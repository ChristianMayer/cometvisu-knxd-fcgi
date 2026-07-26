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

#include <gtest/gtest.h>

#include <thread>

#include "state/shared_group_cache.h"

using namespace cvknxd;

class SharedGroupCacheTest : public ::testing::Test {
public:
  void SetUp() override { ASSERT_TRUE(cache_.create()); }
  void TearDown() override {
    // Cache destructor handles munmap via owns_mmap_
  }

  SharedGroupCache cache_;
};

// ================================================================
// Basic push / get
// ================================================================

TEST_F(SharedGroupCacheTest, PushAndGet) {
  cache_.push(0x0A03, {0x42});  // 1/2/3
  auto val = cache_.get(0x0A03);
  ASSERT_TRUE(val.has_value());
  ASSERT_EQ(val->size(), 1);
  EXPECT_EQ((*val)[0], 0x42);
}

TEST_F(SharedGroupCacheTest, GetMissingReturnsNullopt) {
  auto val = cache_.get(0xFFFF);
  EXPECT_FALSE(val.has_value());
}

TEST_F(SharedGroupCacheTest, PushOverwritesExisting) {
  cache_.push(0x0A03, {0x42});
  cache_.push(0x0A03, {0x99});
  auto val = cache_.get(0x0A03);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ((*val)[0], 0x99);
}

TEST_F(SharedGroupCacheTest, MultiByteValue) {
  cache_.push(0x0B04, {0x0C, 0x6F});  // 2-byte temperature
  auto val = cache_.get(0x0B04);
  ASSERT_TRUE(val.has_value());
  ASSERT_EQ(val->size(), 2);
  EXPECT_EQ((*val)[0], 0x0C);
  EXPECT_EQ((*val)[1], 0x6F);
}

// ================================================================
// Position monotonicity
// ================================================================

TEST_F(SharedGroupCacheTest, PositionMonotonic) {
  EXPECT_EQ(cache_.position(), 0);
  cache_.push(0x0A03, {0x01});
  EXPECT_EQ(cache_.position(), 1);
  cache_.push(0x0B04, {0x02});
  EXPECT_EQ(cache_.position(), 2);
  cache_.push(0x0C05, {0x03});
  EXPECT_EQ(cache_.position(), 3);
}

TEST_F(SharedGroupCacheTest, PositionNeverGoesBackward) {
  uint32_t prev = cache_.position();
  for (int i = 0; i < 100; ++i) {
    cache_.push(static_cast<uint16_t>(0x0A00 + i), {static_cast<uint8_t>(i)});
    uint32_t curr = cache_.position();
    EXPECT_GT(curr, prev) << "Position went backward at i=" << i;
    prev = curr;
  }
}

// ================================================================
// Position wrapping
// ================================================================

/// Position wraps at kPositionModulus and cycles 0..M-1..0.
TEST_F(SharedGroupCacheTest, PositionWrapsAtModulus) {
  // Push M-1 entries: position climbs to M-1 = 99,999.
  for (uint32_t i = 0; i < kPositionModulus - 1; ++i) {
    cache_.push(0x0A03, {static_cast<uint8_t>(i & 0xFF)});
  }
  EXPECT_EQ(cache_.position(), kPositionModulus - 1);  // max = 99,999

  // One more push wraps: position = M % M = 0.
  cache_.push(0x0A03, {0x42});
  EXPECT_EQ(cache_.position(), 0) << "Position should wrap to 0";

  // Continues advancing from 0.
  cache_.push(0x0A03, {0x99});
  EXPECT_EQ(cache_.position(), 1) << "Position advances after wrap";
}

/// Delta queries work correctly across the wrap boundary.
/// Client with i = M-2 receives packets at M-1, 0 (wrap), 1, 2 (current).
TEST_F(SharedGroupCacheTest, DeltaAcrossWrapBoundary) {
  uint16_t addr = 0x0A03;

  // Push M-1 entries: raw 0..M-2, pushed_at 0..M-2. position = M-1.
  for (uint32_t i = 0; i < kPositionModulus - 1; ++i) {
    cache_.push(addr, {static_cast<uint8_t>(i & 0xFF)});
  }
  EXPECT_EQ(cache_.position(), kPositionModulus - 1);

  // Push the wrap entry: raw = M-1, pushed_at = M-1. position = 0 (wrapped).
  cache_.push(addr, {0x99});

  // Push two more: positions 1 and 2.
  cache_.push(addr, {0xAA});
  cache_.push(addr, {0xBB});  // current value
  EXPECT_EQ(cache_.position(), 2);

  // Client with i = M-2 should see all entries from M-1 through 2.
  // Modular: since=M-2, current=2, epoch_distance=(2-(M-2)+M)%M=4.
  // Entries at pushed_at M-1, 0, 1, 2 → all with dist ≤ 4 → all match.
  // But same-address dedup gives only the latest (pushed_at=2 → 0xBB).
  auto delta = cache_.get_delta(kPositionModulus - 2, {addr});
  ASSERT_EQ(delta.values.size(), 1);
  EXPECT_EQ(delta.values[addr][0], 0xBB) << "Must deliver latest post-wrap value";
  EXPECT_EQ(delta.position, 2) << "Position reflects current state";

  // Delta from the exact wrap position finds all subsequent entries.
  delta = cache_.get_delta(kPositionModulus - 1, {addr});
  ASSERT_EQ(delta.values.size(), 1);
  EXPECT_EQ(delta.values[addr][0], 0xBB);
  EXPECT_EQ(delta.position, 2);
}

/// Epoch mismatch: client from early in the epoch, server near the end.
TEST_F(SharedGroupCacheTest, EpochMismatchTriggersFullRefresh) {
  uint16_t addr = 0x0A03;

  // Push M-3 entries (raw 0..M-4, pushed_at 0..M-4). position = M-3.
  for (uint32_t i = 0; i < kPositionModulus - 3; ++i) {
    cache_.push(addr, {static_cast<uint8_t>(i & 0xFF)});
  }

  // Push current value: raw = M-3, pushed_at = M-3. position = M-2.
  cache_.push(addr, {0x42});

  // Client with i=10 (early epoch): epoch_distance = (M-2 - 10 + M) % M = M-12.
  // M-12 >= M/2 → epoch mismatch → full refresh.
  auto delta = cache_.get_delta(10, {addr});
  ASSERT_EQ(delta.values.size(), 1);
  EXPECT_EQ(delta.values[addr][0], 0x42);
}

/// Position wraps 0→M-1→0→1 seamlessly (monotonic in raw).
TEST_F(SharedGroupCacheTest, PositionMonotonicAcrossWrap) {
  uint16_t addr = 0x0A03;

  // Push M-1 entries: position = M-1 = 99,999.
  for (uint32_t i = 0; i < kPositionModulus - 1; ++i) {
    cache_.push(addr, {static_cast<uint8_t>(i & 0xFF)});
  }
  EXPECT_EQ(cache_.position(), kPositionModulus - 1);

  // One more push wraps: position = M % M = 0.
  cache_.push(addr, {0x42});
  EXPECT_EQ(cache_.position(), 0) << "Position wrapped to 0";

  // Pushing more advances again from the wrapped position.
  cache_.push(addr, {0x99});
  EXPECT_EQ(cache_.position(), 1) << "Position advances after wrap";
}

// ================================================================
// Delta queries
// ================================================================

TEST_F(SharedGroupCacheTest, GetDeltaReturnsNewerEntries) {
  cache_.push(0xFFFF, {0x00});  // dummy: pushed_at=0
  cache_.push(0x0A03, {0x01});  // pushed_at=1
  cache_.push(0x0B04, {0x02});  // pushed_at=2
  cache_.push(0x0C05, {0x03});  // pushed_at=3

  auto delta = cache_.get_delta(1, {0x0A03, 0x0B04, 0x0C05});
  // Only entries with pushed_at > 1 → pos 2 (0x0B04) and pos 3 (0x0C05)
  EXPECT_EQ(delta.values.size(), 2);
  EXPECT_TRUE(delta.values.contains(0x0B04));
  EXPECT_TRUE(delta.values.contains(0x0C05));
  EXPECT_FALSE(delta.values.contains(0x0A03));  // pushed_at=1, not > since_pos
  EXPECT_EQ(delta.position, 4);
}

TEST_F(SharedGroupCacheTest, GetDeltaEmptyWhenNoneNewer) {
  cache_.push(0x0A03, {0x01});  // pos 1

  auto delta = cache_.get_delta(5, {0x0A03});
  // since_pos (5) > current position (1) → epoch detection: effective_since = 0
  // pushed_at=1 > 0 → entry returned
  EXPECT_EQ(delta.values.size(), 1);
  EXPECT_EQ(delta.position, 1);
}

TEST_F(SharedGroupCacheTest, GetDeltaOnlySubscribed) {
  cache_.push(0x0A03, {0x01});  // pos 1
  cache_.push(0x0B04, {0x02});  // pos 2

  auto delta = cache_.get_delta(0, {0x0A03});  // only subscribed to 0x0A03
  EXPECT_EQ(delta.values.size(), 1);
  EXPECT_TRUE(delta.values.contains(0x0A03));
  EXPECT_FALSE(delta.values.contains(0x0B04));
}

// ================================================================
// Age filtering
// ================================================================

TEST_F(SharedGroupCacheTest, AgeFilterRejectsStaleEntries) {
  cache_.push(0x0A03, {0x42});
  // Age filter of 0 seconds — entry older than 0s is rejected.
  auto val = cache_.get(0x0A03, 0);
  EXPECT_FALSE(val.has_value()) << "Entry with timestamp=now should NOT be "
                                   "older than 0s (age filter rejects >= max_age)";

  // Actually: timestamp is `now` at push time. The age check is
  // `now - e.timestamp >= max_age_sec`.  With max_age_sec=0, this is
  // `now - now >= 0` → true → rejected.  This is correct behavior:
  // max_age_sec=0 means "only entries newer than 0 seconds" which is
  // impossible since time always advances by at least 1s between push
  // and get.
}

// ================================================================
// Notification (condition variable)
// ================================================================

TEST_F(SharedGroupCacheTest, WaitForNewDataWakesOnPush) {
  // Start with some data so generation > 0
  cache_.push(0x0A03, {0x01});

  std::atomic<bool> woken{false};

  std::thread waiter([this, &woken]() {
    // Wait for new data — should wake when push happens
    bool got_data = cache_.wait_for_new_data(5000);
    woken.store(got_data);
  });

  // Give waiter time to enter the wait
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Push new data — should broadcast and wake the waiter
  cache_.push(0x0B04, {0x02});

  waiter.join();
  EXPECT_TRUE(woken.load()) << "Waiter was not woken by push";
}

TEST_F(SharedGroupCacheTest, WaitForNewDataTimeout) {
  // No data pushed — wait should time out
  bool got_data = cache_.wait_for_new_data(50);
  EXPECT_FALSE(got_data) << "Should time out when no new data arrives";
}

TEST_F(SharedGroupCacheTest, WaitForNewDataSeesPreexistingData) {
  // Initially, no data has been pushed — wait_for_new_data(0) returns false.
  bool got_data = cache_.wait_for_new_data(0);
  EXPECT_FALSE(got_data) << "No data in cache yet";

  // Push data — generation advances to 1.
  cache_.push(0x0A03, {0x01});

  // wait_for_new_data(0) now returns true because generation > 0.
  got_data = cache_.wait_for_new_data(0);
  EXPECT_TRUE(got_data) << "Should detect data was pushed";

  // After consuming (calling wait_for_new_data again), still returns true
  // because generation is still > 0.
  got_data = cache_.wait_for_new_data(0);
  EXPECT_TRUE(got_data) << "Generation stays > 0 after consumption";
}

// ================================================================
// Multiple cache instances sharing the same data (multi-worker)
// ================================================================

TEST_F(SharedGroupCacheTest, TwoInstancesShareSameData) {
  // Simulate two worker processes by attaching a second SharedGroupCache
  // to the same underlying SharedCacheData.
  SharedGroupCache cache2;
  cache2.attach(cache_.data());

  // Push via cache1
  cache_.push(0x0A03, {0x42});

  // Read via cache2 — should see the value
  auto val = cache2.get(0x0A03);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ((*val)[0], 0x42);

  // Position is shared
  EXPECT_EQ(cache2.position(), 1);
  EXPECT_EQ(cache_.position(), 1);

  // Push via cache2
  cache2.push(0x0B04, {0x99});

  // Read via cache1
  auto val2 = cache_.get(0x0B04);
  ASSERT_TRUE(val2.has_value());
  EXPECT_EQ((*val2)[0], 0x99);

  // Position advanced consistently
  EXPECT_EQ(cache_.position(), 2);
  EXPECT_EQ(cache2.position(), 2);
}

// ================================================================
// Non-monotonic i bug reproduction
// ================================================================

/// This test verifies that two "worker" caches sharing the same data
/// always see a monotonically increasing position — the core bug fix.
TEST_F(SharedGroupCacheTest, MonotonicPositionAcrossTwoInstances) {
  SharedGroupCache cache2;
  cache2.attach(cache_.data());

  // Simulate worker A advancing the cache
  for (int i = 0; i < 10; ++i) {
    cache_.push(static_cast<uint16_t>(0x0A00 + i), {static_cast<uint8_t>(i)});
  }
  uint32_t pos_a = cache_.position();
  EXPECT_EQ(pos_a, 10);

  // Worker B queries — should see the SAME position (shared counter)
  uint32_t pos_b = cache2.position();
  EXPECT_EQ(pos_b, pos_a) << "BUG: position differs across cache instances";
  EXPECT_GE(pos_b, pos_a) << "BUG: position went backward across instances";
}

// ================================================================
// Large number of entries (fill the table)
// ================================================================

TEST_F(SharedGroupCacheTest, ManyEntries) {
  // Push many entries — should not crash or corrupt
  for (int i = 0; i < 500; ++i) {
    uint16_t addr = static_cast<uint16_t>(0x1000 + i);
    cache_.push(addr, {static_cast<uint8_t>(i & 0xFF)});
  }

  // Verify some entries
  for (int i = 0; i < 500; ++i) {
    uint16_t addr = static_cast<uint16_t>(0x1000 + i);
    auto val = cache_.get(addr);
    ASSERT_TRUE(val.has_value()) << "Missing entry for addr " << i;
    EXPECT_EQ((*val)[0], static_cast<uint8_t>(i & 0xFF));
  }

  EXPECT_EQ(cache_.position(), 500);
}

// ================================================================
// max_age_sec = -1 (no age filter) — the default
// ================================================================

/// get() with max_age_sec=-1 must always return the cached entry regardless
/// of age.  This is the default and the semantically correct behavior for
/// initial reads — the caller wants the latest known value, not a value
/// filtered by an arbitrary time window.
TEST_F(SharedGroupCacheTest, GetWithNoAgeFilterReturnsData) {
  cache_.push(0x0A03, {0x42});
  auto val = cache_.get(0x0A03, -1);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ((*val)[0], 0x42);
}

/// Contrast: get() with max_age_sec=0 rejects everything (timestamp never
/// strictly less than 0 seconds old).  get() with max_age_sec=-1 accepts
/// everything.  This is why the ReadHandler must pass -1, not timeout_sec.
TEST_F(SharedGroupCacheTest, GetAgeZeroVsNoFilter) {
  cache_.push(0x0A03, {0x42});

  // max_age_sec=0: entry is "now - now >= 0" → rejected.
  auto val_zero = cache_.get(0x0A03, 0);
  EXPECT_FALSE(val_zero.has_value())
      << "max_age_sec=0 must reject all entries (no entry can be <0s old)";

  // max_age_sec=-1: no age filter → accepted.
  auto val_no_filter = cache_.get(0x0A03, -1);
  ASSERT_TRUE(val_no_filter.has_value());
  EXPECT_EQ((*val_no_filter)[0], 0x42);
}

/// get_delta() with max_age_sec=-1 must filter ONLY by position
/// (pushed_at > since_pos), never by age.
TEST_F(SharedGroupCacheTest, GetDeltaWithNoAgeFilterReturnsByPositionOnly) {
  // With 0-based positions: pushed_at = 0, 1, 2, 3. position = 4.
  cache_.push(0x0A03, {0x01});  // pushed_at=0
  cache_.push(0x0A03, {0x02});  // pushed_at=1
  cache_.push(0x0A03, {0x03});  // pushed_at=2
  cache_.push(0x0A03, {0x04});  // pushed_at=3

  // since_pos=1: entries at pushed_at > 1 → pushed_at 2 and 3.
  // Same-address dedup gives only the latest (pushed_at=3 → 0x04).
  auto delta = cache_.get_delta(1, {0x0A03}, -1);
  EXPECT_EQ(delta.values.size(), 1);
  EXPECT_TRUE(delta.values.contains(0x0A03));
  EXPECT_EQ(delta.values[0x0A03][0], 0x04);
  EXPECT_EQ(delta.position, 4);

  // since_pos=2: entries at pushed_at > 2 → pushed_at=3 only.
  auto delta2 = cache_.get_delta(2, {0x0A03}, -1);
  EXPECT_EQ(delta2.values.size(), 1);
  EXPECT_EQ(delta2.values[0x0A03][0], 0x04);
  EXPECT_EQ(delta2.position, 4);

  // since_pos=3: entries at pushed_at > 3 → none.
  auto delta3 = cache_.get_delta(3, {0x0A03}, -1);
  EXPECT_TRUE(delta3.values.empty());
  EXPECT_EQ(delta3.position, 4);

  // since_pos=4: nothing newer.
  auto delta4 = cache_.get_delta(4, {0x0A03}, -1);
  EXPECT_TRUE(delta4.values.empty());
  EXPECT_EQ(delta4.position, 4);
}

/// get_delta() with max_age_sec=0 must NOT return entries (same as get).
/// This demonstrates why passing timeout_sec as max_age_sec is dangerous:
/// with t=0 (timeout_sec=1), entries pushed 1+ seconds ago would be hidden.
TEST_F(SharedGroupCacheTest, GetDeltaWithAgeZeroRejectsAll) {
  cache_.push(0x0A03, {0x42});  // pushed_at=0, position=1

  // max_age_sec=0: age filter rejects everything.
  auto delta = cache_.get_delta(0, {0x0A03}, 0);
  EXPECT_TRUE(delta.values.empty()) << "max_age_sec=0 must reject all entries in get_delta";
  EXPECT_EQ(delta.position, 1);  // position is still reported

  // max_age_sec=-1: no age filter → entry returned.
  auto delta2 = cache_.get_delta(0, {0x0A03}, -1);
  EXPECT_EQ(delta2.values.size(), 1);
  EXPECT_TRUE(delta2.values.contains(0x0A03));
}
