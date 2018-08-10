/*
 *  Copyright (c) 2014-present, Facebook, Inc.
 *
 *  This source code is licensed under the MIT license found in the LICENSE
 *  file in the root directory of this source tree.
 *
 */
#include "TkoTracker.h"

#include <cassert>
#include <functional>

#include <folly/MapUtil.h>

#include "mcrouter/ProxyDestination.h"
#include "mcrouter/TkoCounters.h"

namespace facebook {
namespace memcache {
namespace mcrouter {

TkoTracker::TkoTracker(
    size_t tkoThreshold,
    TkoTrackerMap& trackerMap)
    : tkoThreshold_(tkoThreshold),
      trackerMap_(trackerMap) {}

bool TkoTracker::isHardTko() const {
  uintptr_t curSumFailures = sumFailures_;
  return (curSumFailures > tkoThreshold_ && curSumFailures % 2 == 1);
}

bool TkoTracker::isSoftTko() const {
  uintptr_t curSumFailures = sumFailures_;
  return (curSumFailures > tkoThreshold_ && curSumFailures % 2 == 0);
}

const TkoCounters& TkoTracker::globalTkos() const {
  return trackerMap_.globalTkos_;
}

void TkoTracker::incrementSoftTkoCount() {
  trackerMap_.globalTkos_.softTkos++;
}

void TkoTracker::decrementSoftTkoCount() {
  // Decrement the counter and ensure we haven't gone below 0
  size_t oldSoftTkos = trackerMap_.globalTkos_.softTkos.fetch_sub(1);
  assert(oldSoftTkos != 0);
}

bool TkoTracker::setSumFailures(uintptr_t value) {
  uintptr_t curSumFailures = sumFailures_;
  do {
    /* If the destination is TKO but we're not responsible we can't change
       state, so return. */
    if (curSumFailures > tkoThreshold_) {
      return false;
    }
  } while (!sumFailures_.compare_exchange_weak(curSumFailures, value));
  return true;
}

bool TkoTracker::recordSoftFailure(ProxyDestination* pdstn) {
  // We increment soft tko count first before actually taking responsibility
  // for the TKO. This means we run the risk that multiple proxies
  // increment the count for the same destination, causing us to be overly
  // conservative. Eventually this will get corrected, as only one proxy can
  // ever mark it TKO, but we may be inconsistent for a very short time.
  ++consecutiveFailureCount_;

  // If host is in any state of TKO, we just leave it alone
  if (isTko()) {
    return false;
  }

  uintptr_t curSumFailures = sumFailures_;
  uintptr_t value = 0;
  uintptr_t pdstnAddr = reinterpret_cast<uintptr_t>(pdstn);
  do {
    // If we're one failure below the limit, we're about to enter softTKO
    if (curSumFailures == tkoThreshold_ - 1) {
      // Note: we need to check value to ensure we didn't already increment
      // the counter in a previous iteration
      if (value != pdstnAddr) {
        incrementSoftTkoCount();
      }
      value = pdstnAddr;
    } else {
      if (value == pdstnAddr) {
        // a previous loop iteration attempted to soft TKO the box,
        // so we need to undo that
        decrementSoftTkoCount();
      }
      // Someone else is responsible, so quit
      if (curSumFailures > tkoThreshold_) {
        return false;
      } else {
        value = curSumFailures + 1;
      }
    }
  } while (!sumFailures_.compare_exchange_weak(curSumFailures, value));
  return value == pdstnAddr;
}

bool TkoTracker::recordHardFailure(ProxyDestination* pdstn) {
  ++consecutiveFailureCount_;

  if (isHardTko()) {
    return false;
  }
  // If we were already TKO and responsible, but not hard TKO, it means we were
  // in soft TKO before. We need decrement the counter and convert to hard
  // TKO
  if (isResponsible(pdstn)) {
    // convert to hard failure
    sumFailures_ |= 1;
    decrementSoftTkoCount();
    ++trackerMap_.globalTkos_.hardTkos;
    // We've already been marked responsible
    return false;
  }

  // If the call below succeeds we marked the box TKO and took responsibility.
  bool success = setSumFailures(reinterpret_cast<uintptr_t>(pdstn) | 1);
  if (success) {
    ++trackerMap_.globalTkos_.hardTkos;
  }
  return success;
}

bool TkoTracker::isResponsible(ProxyDestination* pdstn) const {
  return (sumFailures_ & ~1) == reinterpret_cast<uintptr_t>(pdstn);
}

bool TkoTracker::recordSuccess(ProxyDestination* pdstn) {
  // If we're responsible, no one else can change any state and we're
  // effectively under mutex.
  if (isResponsible(pdstn)) {
    // Coming out of TKO, we need to decrement counters
    if (isSoftTko()) {
      decrementSoftTkoCount();
    }
    if (isHardTko()) {
      --trackerMap_.globalTkos_.hardTkos;
    }
    sumFailures_ = 0;
    consecutiveFailureCount_ = 0;
    return true;
  }

  // Skip resetting failures if the counter is at zero.
  // If an error races here and increments the counter,
  // we can pretend this success happened before the error,
  // and the state is consistent.

  // If we don't skip here we end up doing CAS on a shared state
  // every single request.
  if (sumFailures_ != 0 && setSumFailures(0)) {
    consecutiveFailureCount_ = 0;
  }
  return false;
}

bool TkoTracker::removeDestination(ProxyDestination* pdstn) {
  // We should clear the TKO state if pdstn is responsible
  if (isResponsible(pdstn)) {
    return recordSuccess(pdstn);
  }
  return false;
}

TkoTracker::~TkoTracker() {
  trackerMap_.removeTracker(key_);
}

void TkoTrackerMap::updateTracker(
    ProxyDestination& pdstn,
    const size_t tkoThreshold) {
  auto key = pdstn.accessPoint()->toHostPortString();

  // This shared pointer has to be destroyed after releasing "mx_".
  // The reason is that when "tracker" goes out of scope, we might destroy
  // the TkoTracker it points to, which would again try to lock "mx_"
  // (triggering an UB).
  std::shared_ptr<TkoTracker> tracker;
  {
    std::lock_guard<std::mutex> lock(mx_);
    auto it = trackers_.find(key);
    if (it == trackers_.end() || (tracker = it->second.lock()) == nullptr) {
      tracker.reset(new TkoTracker(tkoThreshold, *this));
      auto trackerIt = trackers_.emplace(key, tracker);
      if (!trackerIt.second) {
        trackerIt.first->second = tracker;
      }
      tracker->key_ = trackerIt.first->first;
    }
  }
  pdstn.tracker = std::move(tracker);
}

std::unordered_map<std::string, std::pair<bool, size_t>>
TkoTrackerMap::getSuspectServers() const {
  std::unordered_map<std::string, std::pair<bool, size_t>> result;
  foreachTkoTracker(
      [&result](folly::StringPiece key, const TkoTracker& tracker) mutable {
        auto failures = tracker.consecutiveFailureCount();
        if (failures > 0) {
          result.emplace(key.str(), std::make_pair(tracker.isTko(), failures));
        }
      });
  return result;
}

size_t TkoTrackerMap::getSuspectServersCount() const {
  size_t result = 0;
  foreachTkoTracker(
      [&result](folly::StringPiece, const TkoTracker& tracker) mutable {
        if (tracker.consecutiveFailureCount() > 0) {
          ++result;
        }
      });
  return result;
}

void TkoTrackerMap::foreachTkoTracker(
    std::function<void(folly::StringPiece, const TkoTracker&)> func) const {
  // We allocate a vector to delay the desctruction of TkoTrackers until after
  // we release the lock. This is done to avoid the following race:
  // After successfully locking the weak_ptr, we might become the last
  // holder of the shared_ptr to TkoTracker. If that's the case, it would
  // trigger destruction of TkoTracker, which would try to grab "mx_" and
  // remove itself from the map.
  // If we didn't have the lock, this would case two issues:
  //  - We would try to lock "mx_" again from the same thread (which is UB).
  //  - We would change "trackers_" while iterating over it.
  std::vector<std::shared_ptr<TkoTracker>> lockedTrackers;
  lockedTrackers.reserve(trackers_.size());

  {
    std::lock_guard<std::mutex> lock(mx_);
    for (const auto& it : trackers_) {
      if (auto tracker = it.second.lock()) {
        func(it.first, *tracker);
        lockedTrackers.emplace_back(std::move(tracker));
      }
    }
  }
}

void TkoTrackerMap::removeTracker(folly::StringPiece key) noexcept {
  std::lock_guard<std::mutex> lock(mx_);
  auto it = trackers_.find(key);
  if (it != trackers_.end()) {
    trackers_.erase(it);
  }
}

} // mcrouter
} // memcache
} // facebook
