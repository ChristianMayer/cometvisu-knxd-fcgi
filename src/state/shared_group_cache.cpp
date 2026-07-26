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

#include "shared_group_cache.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace cvknxd {

SharedGroupCache::~SharedGroupCache() {
  if (data_ != nullptr && owns_mmap_) {
    ::munmap(data_, sizeof(SharedCacheData));
    data_ = nullptr;
    owns_mmap_ = false;
  }
}

bool SharedGroupCache::create() {
  if (data_ != nullptr) {
    return false;  // already created
  }

  void* region = ::mmap(nullptr, sizeof(SharedCacheData), PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (region == MAP_FAILED) {
    return false;
  }

  // Placement-new the SharedCacheData to initialize all fields to zero.
  auto* d = new (region) SharedCacheData{};

  // Initialize the process-shared mutex.
  ::pthread_mutexattr_t mutex_attr;
  ::pthread_mutexattr_init(&mutex_attr);
  ::pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
  // Use robust mutex so that if the cache reader crashes while holding
  // the lock, other processes can recover (EOWNERDEAD).
  ::pthread_mutexattr_setrobust(&mutex_attr, PTHREAD_MUTEX_ROBUST);
  if (::pthread_mutex_init(&d->mutex, &mutex_attr) != 0) {
    ::pthread_mutexattr_destroy(&mutex_attr);
    ::munmap(region, sizeof(SharedCacheData));
    return false;
  }
  ::pthread_mutexattr_destroy(&mutex_attr);

  // Initialize the process-shared condition variable.
  ::pthread_condattr_t cond_attr;
  ::pthread_condattr_init(&cond_attr);
  ::pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED);
  // Use monotonic clock for timeout calculations.
  ::pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
  if (::pthread_cond_init(&d->cond, &cond_attr) != 0) {
    ::pthread_condattr_destroy(&cond_attr);
    ::pthread_mutex_destroy(&d->mutex);
    ::munmap(region, sizeof(SharedCacheData));
    return false;
  }
  ::pthread_condattr_destroy(&cond_attr);

  data_ = d;
  owns_mmap_ = true;
  return true;
}

void SharedGroupCache::attach(SharedCacheData* data) {
  data_ = data;
  owns_mmap_ = false;
}

// ---- Internal helpers ----

size_t SharedGroupCache::find_slot(uint16_t addr) const {
  if (data_ == nullptr) {
    return kSharedCacheCapacity;
  }
  // Simple open addressing with linear probing.
  size_t index = static_cast<size_t>(addr) % kSharedCacheCapacity;
  for (size_t i = 0; i < kSharedCacheCapacity; ++i) {
    size_t slot = (index + i) % kSharedCacheCapacity;
    if (data_->entries[slot].addr == addr || data_->entries[slot].addr == 0) {
      return slot;
    }
  }
  return kSharedCacheCapacity;  // table full
}

void SharedGroupCache::insert_locked(uint16_t addr, const SharedCacheEntry& entry) {
  size_t slot = find_slot(addr);
  if (slot >= kSharedCacheCapacity) {
    return;  // table full — drop entry
  }

  bool is_new = (data_->entries[slot].addr == 0);
  data_->entries[slot] = entry;
  if (is_new) {
    data_->num_entries++;
  }
}

// ---- Public API ----

void SharedGroupCache::push(uint16_t addr, const std::vector<uint8_t>& value) {
  if (data_ == nullptr) {
    return;
  }

  int lock_result = ::pthread_mutex_lock(&data_->mutex);
  if (lock_result == EOWNERDEAD) {
    // Previous owner died while holding the lock — make it consistent.
    ::pthread_mutex_consistent(&data_->mutex);
  } else if (lock_result != 0) {
    return;
  }

  // Increment the raw counter and store the wrapped position so the client-facing
  // index `i` stays bounded.  Wrapped values are 0-based (0..kPositionModulus-1)
  // so they match the position() return value and the client's `i` parameter.
  uint32_t raw_pos = data_->position.fetch_add(1);
  uint32_t wrapped_pos = raw_pos % kPositionModulus;

  SharedCacheEntry entry{};
  entry.addr = addr;
  entry.value_len = static_cast<uint16_t>(std::min(value.size(), kMaxSharedValueBytes));
  entry.timestamp = static_cast<uint32_t>(std::time(nullptr));
  entry.pushed_at = wrapped_pos;
  std::memcpy(entry.value, value.data(), entry.value_len);

  insert_locked(addr, entry);

  // Increment generation to signal new data.
  data_->generation.fetch_add(1);

  ::pthread_mutex_unlock(&data_->mutex);

  // Wake all blocked long-poll readers.
  notify_all();
}

std::optional<std::vector<uint8_t>> SharedGroupCache::get(uint16_t addr, int max_age_sec) const {
  if (data_ == nullptr) {
    return std::nullopt;
  }

  int lock_result = ::pthread_mutex_lock(&data_->mutex);
  if (lock_result == EOWNERDEAD) {
    ::pthread_mutex_consistent(&data_->mutex);
  } else if (lock_result != 0) {
    return std::nullopt;
  }

  std::optional<std::vector<uint8_t>> result;
  size_t slot = find_slot(addr);
  if (slot < kSharedCacheCapacity && data_->entries[slot].addr == addr) {
    const auto& e = data_->entries[slot];
    if (max_age_sec >= 0) {
      auto now = static_cast<uint32_t>(std::time(nullptr));
      if (now - e.timestamp < static_cast<uint32_t>(max_age_sec)) {
        result.emplace(std::begin(e.value), std::next(std::begin(e.value), e.value_len));
      }
    } else {
      result.emplace(std::begin(e.value), std::next(std::begin(e.value), e.value_len));
    }
  }

  ::pthread_mutex_unlock(&data_->mutex);
  return result;
}

SharedGroupCache::Delta SharedGroupCache::get_delta(uint32_t since_pos,
                                                    const std::set<uint16_t>& subscribed,
                                                    int max_age_sec) const {
  Delta delta;
  if (data_ == nullptr) {
    return delta;
  }

  int lock_result = ::pthread_mutex_lock(&data_->mutex);
  if (lock_result == EOWNERDEAD) {
    ::pthread_mutex_consistent(&data_->mutex);
  } else if (lock_result != 0) {
    return delta;
  }

  // Current wrapped position (0-based, wraps at kPositionModulus).
  delta.position = data_->position.load() % kPositionModulus;

  // Modular distance from the client's last-seen position to the current position.
  // This tells us how far ahead (in modular space) the cache has advanced since
  // the client last polled.
  uint32_t epoch_distance = (delta.position - since_pos + kPositionModulus) % kPositionModulus;

  // Epoch detection: if the distance is >= half the modulus, the client is from
  // a previous wrap-around epoch.  In that case we return all current values for
  // the subscribed addresses (full refresh) instead of a delta.
  // Epoch mismatch when the client is more than half the modulus away.
  // With M=100_000, this threshold = 50_000 entries (~17 min at 50 tps),
  // far exceeding the 300-second long-poll timeout.
  bool epoch_mismatch = (since_pos != 0 && epoch_distance >= kPositionModulus / 2);

  uint32_t now = static_cast<uint32_t>(std::time(nullptr));

  for (uint16_t addr : subscribed) {
    size_t slot = find_slot(addr);
    if (slot >= kSharedCacheCapacity) {
      continue;
    }
    const auto& e = data_->entries[slot];
    if (e.addr != addr) {
      continue;
    }
    if (max_age_sec >= 0 && now - e.timestamp >= static_cast<uint32_t>(max_age_sec)) {
      continue;
    }

    if (epoch_mismatch) {
      // Full refresh: return all subscribed values regardless of pushed_at.
      delta.values[addr] =
          std::vector<uint8_t>(std::begin(e.value), std::next(std::begin(e.value), e.value_len));
    } else {
      // Normal delta: only include entries newer than since_pos (modular).
      uint32_t entry_dist = (e.pushed_at - since_pos + kPositionModulus) % kPositionModulus;
      // An entry is newer if its distance is positive and within epoch_distance.
      // When since_pos=0 (new client), pushed_at=0 is valid first-push data.
      if (entry_dist > epoch_distance) {
        continue;
      }
      if (since_pos != 0 && entry_dist == 0) {
        continue;
      }
      delta.values[addr] =
          std::vector<uint8_t>(std::begin(e.value), std::next(std::begin(e.value), e.value_len));
    }
  }

  ::pthread_mutex_unlock(&data_->mutex);
  return delta;
}

bool SharedGroupCache::wait_for_new_data(int timeout_ms) {
  if (data_ == nullptr) {
    return false;
  }

  int lock_result = ::pthread_mutex_lock(&data_->mutex);
  if (lock_result == EOWNERDEAD) {
    ::pthread_mutex_consistent(&data_->mutex);
  } else if (lock_result != 0) {
    return false;
  }

  uint32_t gen_before = data_->generation.load();

  if (timeout_ms <= 0) {
    // Zero timeout: non-blocking check — has the cache ever received data?
    // This is useful for poll loops that want to check without blocking.
    bool has_data = data_->generation.load() > 0;
    ::pthread_mutex_unlock(&data_->mutex);
    return has_data;
  }

  // Compute absolute timeout using monotonic clock.
  struct timespec ts {};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  int64_t total_ns =
      static_cast<int64_t>(ts.tv_nsec) + static_cast<int64_t>(timeout_ms) * 1'000'000LL;
  ts.tv_sec += static_cast<time_t>(total_ns / 1'000'000'000LL);
  ts.tv_nsec = static_cast<decltype(ts.tv_nsec)>(total_ns % 1'000'000'000LL);

  // Wait loop: handles spurious wakeups.
  bool timed_out = false;
  while (data_->generation.load() == gen_before && !timed_out) {
    int rc = ::pthread_cond_timedwait(&data_->cond, &data_->mutex, &ts);
    if (rc == ETIMEDOUT) {
      timed_out = true;
    } else if (rc != 0) {
      break;  // error
    }
  }

  uint32_t gen_after = data_->generation.load();
  ::pthread_mutex_unlock(&data_->mutex);

  return gen_after != gen_before;
}

void SharedGroupCache::notify_all() {
  if (data_ != nullptr) {
    // No need to hold the mutex for broadcast (POSIX allows it).
    ::pthread_cond_broadcast(&data_->cond);
  }
}

}  // namespace cvknxd
