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

/**
 * @file shared_group_cache.h
 * @brief Shared-memory KNX group telegram cache with process-shared synchronization.
 *
 * Lives in mmap(MAP_SHARED|MAP_ANONYMOUS) memory so all worker processes
 * see the same data.  A dedicated cache reader process drains the single
 * knxd group socket and pushes telegrams here.  Worker processes query
 * the cache directly — no per-worker knxd group socket needed.
 *
 * Synchronization:
 *   - pthread_mutex_t (PTHREAD_PROCESS_SHARED) protects all entries
 *   - pthread_cond_t (PTHREAD_PROCESS_SHARED) wakes blocked long-poll readers
 *   - std::atomic<uint32_t> position — monotonically increasing, shared
 *   - generation counter — lets waiters detect new data efficiently
 *
 * The data structure is a fixed-size open-addressing hash table (POD,
 * no heap allocation) so it can live entirely in the mmap'd region.
 */

#ifndef COMETVISU_KNXD_FCGI_SHARED_GROUP_CACHE_H_
#define COMETVISU_KNXD_FCGI_SHARED_GROUP_CACHE_H_

#include <atomic>
#include <cstdint>
#include <ctime>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

namespace cvknxd {

/// Maximum number of cache entries (open-addressing hash table).
inline constexpr size_t kSharedCacheCapacity = 2048;

/// Position modulus — the index `i` wraps at this value to keep the per-request
/// transmitted bytes small while covering at least 24 hours of full-speed KNX bus
/// traffic (~50 telegrams/sec).  At 50 tps this gives ~55 hours before the half-range
/// ambiguity kicks in; at 100 tps ~28 hours.  The half-range rule means a client
/// that polls at least once every M/2 entries will always get correct deltas.
/// With the 300-second long-poll timeout this is easily satisfied.
inline constexpr uint32_t kPositionModulus = 100'000;

/// Maximum KNX APDU payload bytes storable per entry.
inline constexpr size_t kMaxSharedValueBytes = 14;

/// One cache entry — must be trivially copyable (POD) for shared memory.
struct SharedCacheEntry {
  uint16_t addr = 0;                         // KNX group address (16-bit EIB)
  uint16_t value_len = 0;                    // actual value bytes used
  uint32_t timestamp = 0;                    // Unix epoch seconds
  uint32_t pushed_at = 0;                    // cache position when pushed
  uint8_t value[kMaxSharedValueBytes] = {};  // NOLINT(modernize-avoid-c-arrays)
};
static_assert(std::is_trivially_copyable_v<SharedCacheEntry>,
              "SharedCacheEntry must be POD for shared memory");

/// The entire shared-memory region.
/// Allocated once via mmap, placement-new'd to initialize mutex/cond.
struct SharedCacheData {
  alignas(64) pthread_mutex_t mutex;
  alignas(64) pthread_cond_t cond;
  std::atomic<uint32_t> position{0};
  std::atomic<uint32_t> generation{0};               // incremented on each push
  uint32_t num_entries{0};                           // current count (≤ kSharedCacheCapacity)
  SharedCacheEntry entries[kSharedCacheCapacity]{};  // NOLINT(modernize-avoid-c-arrays)
};

/**
 * @brief Shared-memory group cache accessible by all worker processes.
 *
 * Wraps a SharedCacheData region in mmap'd memory.  The creator calls
 * create() which mmaps and initializes the mutex/condvar.  Other processes
 * call attach() with the existing data pointer.
 *
 * Thread/process safety:
 *   - All entry access is protected by the process-shared mutex
 *   - wait_for_new_data() blocks on the process-shared condition variable
 *   - position() is lock-free (atomic read)
 */
class SharedGroupCache {
public:
  SharedGroupCache() = default;
  ~SharedGroupCache();

  // Non-copyable, non-movable (owns external mmap reference).
  SharedGroupCache(const SharedGroupCache&) = delete;
  SharedGroupCache& operator=(const SharedGroupCache&) = delete;
  SharedGroupCache(SharedGroupCache&&) = delete;
  SharedGroupCache& operator=(SharedGroupCache&&) = delete;

  /// Create the shared memory region and initialize synchronization primitives.
  /// Must be called once by the parent process before fork().
  /// @return true on success.
  [[nodiscard]] bool create();

  /// Attach to an already-created shared data region (for tests or child processes).
  /// @param data Pointer to initialized SharedCacheData.
  void attach(SharedCacheData* data);

  /// Whether the cache is initialized (create() or attach() succeeded).
  [[nodiscard]] bool is_initialized() const { return data_ != nullptr; }

  /// Push a value.  Called by the cache reader process.
  /// Increments position and generation, broadcasts condition variable.
  void push(uint16_t addr, const std::vector<uint8_t>& value);

  /// Get latest value for an address, optionally age-filtered.
  /// Called by worker processes.
  [[nodiscard]] std::optional<std::vector<uint8_t>> get(uint16_t addr, int max_age_sec = -1) const;

  /// Entries newer than since_pos for subscribed addresses.
  struct Delta {
    std::unordered_map<uint16_t, std::vector<uint8_t>> values;
    uint32_t position = 0;
  };
  /// Only entries with pushed_at newer than since_pos are returned (modular comparison).
  /// If the client is from a previous epoch (modular distance ≥ kPositionModulus / 2),
  /// all current values for subscribed addresses are returned instead (full refresh).
  /// Called by worker processes.
  [[nodiscard]] Delta get_delta(uint32_t since_pos, const std::set<uint16_t>& subscribed,
                                int max_age_sec = -1) const;

  /// Current position (lock-free, wraps at kPositionModulus).
  /// Returns the pushed_at of the most recently pushed entry (0-based),
  /// or 0 if no entries have been pushed yet.  After kPositionModulus
  /// pushes the position wraps back to 0.
  [[nodiscard]] uint32_t position() const { return data_->position.load() % kPositionModulus; }

  /// Block until new data arrives or timeout expires.
  /// Uses the process-shared condition variable — zero CPU while waiting.
  /// @param timeout_ms Maximum wait time in milliseconds.
  /// @return true if new data arrived, false on timeout.
  [[nodiscard]] bool wait_for_new_data(int timeout_ms);

  /// Broadcast to all waiters that new data is available.
  /// Called by the cache reader after push().
  void notify_all();

  /// Get the raw data pointer (for passing to child processes).
  [[nodiscard]] SharedCacheData* data() { return data_; }

  /// Size of the shared memory region in bytes.
  [[nodiscard]] static size_t region_size() { return sizeof(SharedCacheData); }

private:
  SharedCacheData* data_ = nullptr;
  bool owns_mmap_ = false;  // true if we created (and must unmap) the region

  /// Find an entry slot for the given address (open addressing).
  /// Returns index or kSharedCacheCapacity if full.
  [[nodiscard]] size_t find_slot(uint16_t addr) const;

  /// Insert a new entry.  Caller must hold the mutex.
  void insert_locked(uint16_t addr, const SharedCacheEntry& entry);
};

}  // namespace cvknxd

#endif  // COMETVISU_KNXD_FCGI_SHARED_GROUP_CACHE_H_
