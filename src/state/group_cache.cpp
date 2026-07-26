// Copyright (C) 2026 Christian Mayer and the CometVisu contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "group_cache.h"

namespace cvknxd {

void GroupCache::push(uint16_t addr, const std::vector<uint8_t>& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  // Increment the raw counter and store the wrapped position so the client-facing
  // index `i` stays bounded.  Wrapped values are 0-based (0..kPositionModulus-1)
  // so they match the position() return value and the client's `i` parameter.
  uint32_t raw_pos = position_.fetch_add(1);
  uint32_t wrapped_pos = raw_pos % kPositionModulus;
  entries_[addr] = {value, static_cast<uint32_t>(std::time(nullptr)), wrapped_pos};
}

std::optional<std::vector<uint8_t>> GroupCache::get(uint16_t addr, int max_age_sec) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(addr);
  if (it == entries_.end()) {
    return std::nullopt;
  }
  if (max_age_sec >= 0) {
    auto now = static_cast<uint32_t>(std::time(nullptr));
    if (now - it->second.timestamp >= static_cast<uint32_t>(max_age_sec)) {
      return std::nullopt;
    }
  }
  return it->second.value;
}

GroupCache::Delta GroupCache::get_delta(uint32_t since_pos, const std::set<uint16_t>& subscribed,
                                        int max_age_sec) const {
  std::lock_guard<std::mutex> lock(mutex_);
  Delta delta;

  // Current wrapped position (0-based, wraps at kPositionModulus).
  delta.position = position_.load() % kPositionModulus;

  // Modular distance from the client's last-seen position to the current position.
  uint32_t epoch_distance = (delta.position - since_pos + kPositionModulus) % kPositionModulus;

  // Epoch detection: if distance >= half the modulus, the client is from
  // a previous wrap-around epoch.  Return all current values (full refresh).
  // Epoch mismatch when the client is more than half the modulus away.
  // With M=100_000, this threshold = 50_000 entries (~17 min at 50 tps),
  // far exceeding the 300-second long-poll timeout.
  bool epoch_mismatch = (since_pos != 0 && epoch_distance >= kPositionModulus / 2);

  uint32_t now = static_cast<uint32_t>(std::time(nullptr));
  for (auto addr : subscribed) {
    auto it = entries_.find(addr);
    if (it == entries_.end()) {
      continue;
    }
    // Age filter: reject entries older than max_age_sec.
    if (max_age_sec >= 0 && now - it->second.timestamp >= static_cast<uint32_t>(max_age_sec)) {
      continue;
    }

    if (epoch_mismatch) {
      // Full refresh: return all subscribed values regardless of pushed_at.
      delta.values[addr] = it->second.value;
    } else {
      // Normal delta: only include entries newer than since_pos (modular).
      uint32_t entry_dist =
          (it->second.pushed_at - since_pos + kPositionModulus) % kPositionModulus;
      // An entry is newer if its distance is positive and within epoch_distance.
      // When since_pos=0 (new client), pushed_at=0 is valid first-push data.
      if (entry_dist > epoch_distance) {
        continue;
      }
      if (since_pos != 0 && entry_dist == 0) {
        continue;
      }
      delta.values[addr] = it->second.value;
    }
  }
  return delta;
}

void GroupCache::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.clear();
  position_.store(0);
}

}  // namespace cvknxd