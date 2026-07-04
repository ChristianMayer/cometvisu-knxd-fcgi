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

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace cvknxd {

/// Caches the last known value of KNX group addresses with timestamp.
/// Used for the "t" (timeout) parameter in CometVisu read requests.
/// Automatically evicts oldest entries when the cache exceeds max_entries.
class AddressCache {
public:
  /// Default maximum number of cached addresses.
  static constexpr size_t kDefaultMaxEntries = 10000;

  explicit AddressCache(size_t max_entries = kDefaultMaxEntries);

  /// Update the cached value for a group address.
  /// @param eibaddr 16-bit EIB group address.
  /// @param data Raw APDU data bytes.
  void update(uint16_t eibaddr, const std::vector<uint8_t>& data);

  /// Get a cached value if it exists and is not older than max_age.
  /// @param eibaddr 16-bit EIB group address.
  /// @param max_age_seconds Maximum age in seconds (negative = any age).
  /// @return Cached data, or std::nullopt if missing or too old.
  [[nodiscard]] std::optional<std::vector<uint8_t>> get(uint16_t eibaddr,
                                                        int max_age_seconds) const;

  /// Get a value regardless of age.
  /// @return Cached data, or std::nullopt if never cached.
  [[nodiscard]] std::optional<std::vector<uint8_t>> get_any(uint16_t eibaddr) const;

  /// Remove an entry from the cache.
  void remove(uint16_t eibaddr);

  /// Clear all cached values.
  void clear();

  /// Number of cached entries.
  [[nodiscard]] size_t size() const { return cache_.size(); }

private:
  struct CacheEntry {
    std::vector<uint8_t> data;
    std::chrono::steady_clock::time_point timestamp;
  };

  std::unordered_map<uint16_t, CacheEntry> cache_;
  size_t max_entries_;

  /// Evict the oldest entry. Called when cache exceeds max_entries_.
  void evict_one();
};

}  // namespace cvknxd
