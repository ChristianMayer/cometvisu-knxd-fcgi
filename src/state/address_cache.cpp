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

#include "address_cache.h"

namespace cvknxd {

AddressCache::AddressCache(size_t max_entries) : max_entries_(max_entries) {}

void AddressCache::update(uint16_t eibaddr, const std::vector<uint8_t>& data) {
  // Evict if we're at capacity and not overwriting an existing entry
  if (cache_.size() >= max_entries_ && cache_.find(eibaddr) == cache_.end()) {
    evict_one();
  }
  cache_[eibaddr] = CacheEntry{data, std::chrono::steady_clock::now()};
}

void AddressCache::evict_one() {
  if (cache_.empty()) return;

  // Find the oldest entry
  auto oldest = cache_.begin();
  for (auto it = cache_.begin(); it != cache_.end(); ++it) {
    if (it->second.timestamp < oldest->second.timestamp) {
      oldest = it;
    }
  }
  cache_.erase(oldest);
}

std::optional<std::vector<uint8_t>> AddressCache::get(uint16_t eibaddr, int max_age_seconds) const {
  auto it = cache_.find(eibaddr);
  if (it == cache_.end())
    return std::nullopt;

  // Negative max_age means return any cached value regardless of age
  if (max_age_seconds < 0)
    return it->second.data;

  auto age = std::chrono::steady_clock::now() - it->second.timestamp;
  auto age_sec = std::chrono::duration_cast<std::chrono::seconds>(age).count();

  if (age_sec <= max_age_seconds) {
    return it->second.data;
  }

  return std::nullopt;
}

std::optional<std::vector<uint8_t>> AddressCache::get_any(uint16_t eibaddr) const {
  auto it = cache_.find(eibaddr);
  if (it != cache_.end())
    return it->second.data;
  return std::nullopt;
}

void AddressCache::remove(uint16_t eibaddr) {
  cache_.erase(eibaddr);
}

void AddressCache::clear() {
  cache_.clear();
}

}  // namespace cvknxd
