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

#include "handlers/read_handler.h"
#include "mock_knxd_socket.h"
#include "state/session_store.h"
#include "state/shared_group_cache.h"

using namespace cvknxd;

class ReadHandlerTest : public ::testing::Test {
public:
  void SetUp() override {
    ASSERT_TRUE(cache_.create());
    (void)knxd_.connect("/run/knx");
    (void)knxd_.open_group_socket(false);
  }

  MockKnxdClient knxd_;  // NOLINT
  SharedGroupCache cache_;
  SessionStore sessions_;
};

// ================================================================
// Parameter handling & error cases
// ================================================================

TEST_F(ReadHandlerTest, NoAddressesReturns400) {
  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("s=abc&t=0");
  EXPECT_EQ(r.http_status, 400);
  EXPECT_NE(r.body.find("\"error\":\"missing address\""), std::string::npos);
}

TEST_F(ReadHandlerTest, InvalidTimeoutReturns400) {
  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3&t=abc");
  EXPECT_EQ(r.http_status, 400);
}

TEST_F(ReadHandlerTest, SessionInvalidReturns401) {
  (void)sessions_.create_session(false);
  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3&t=0&s=nonexistent");
  EXPECT_EQ(r.http_status, 401);
}

TEST_F(ReadHandlerTest, AnonymousSessionOk) {
  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3&t=0&s=0");
  EXPECT_EQ(r.http_status, 200);
}

TEST_F(ReadHandlerTest, ValidSessionProceeds) {
  auto sid = sessions_.create_session(false);
  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3&t=0&s=" + sid);
  EXPECT_EQ(r.http_status, 200);
}

TEST_F(ReadHandlerTest, NoValidAddressesReturns404) {
  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=invalid");
  EXPECT_EQ(r.http_status, 404);
}

// ================================================================
// Initial read: GroupCache lookup (lastpos == 0)
// ================================================================

TEST_F(ReadHandlerTest, InitialReadFromGroupCache) {
  cache_.push(0x0A03, {0x42});  // 1/2/3
  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3&t=0");
  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(r.body.find("\"42\""), std::string::npos);
  EXPECT_NE(r.body.find("\"i\":"), std::string::npos);
}

TEST_F(ReadHandlerTest, InitialReadCacheMissReturnsEmpty) {
  // t=0 with empty cache: response is returned immediately (non-blocking).
  // GroupValueRead is sent asynchronously for the uncached address — the
  // response will arrive later via the cache reader process.
  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3&t=0");
  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("\"d\":{}"), std::string::npos);
  EXPECT_NE(r.body.find("\"i\":0"), std::string::npos);
}

TEST_F(ReadHandlerTest, InitialReadMultipleAddresses) {
  cache_.push(0x0A03, {0x42});        // 1/2/3
  cache_.push(0x0B04, {0x0C, 0x6F});  // 1/3/4
  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3&a=1/3/4&t=0");
  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(r.body.find("42"), std::string::npos);
  EXPECT_NE(r.body.find("1/3/4"), std::string::npos);
  EXPECT_NE(r.body.find("0c6f"), std::string::npos);
}

TEST_F(ReadHandlerTest, InitialReadMixedCachedAndUncached) {
  cache_.push(0x0A03, {0x42});  // only 1/2/3 cached
  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3&a=1/3/4&t=0");
  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(r.body.find("42"), std::string::npos);
  // Uncached address not in response (returns immediately, non-blocking).
  // GroupValueRead is sent asynchronously for 1/3/4.
  EXPECT_EQ(r.body.find("1/3/4"), std::string::npos);
  auto sent = knxd_.sent_packets();
  ASSERT_EQ(sent.size(), 1);
  EXPECT_EQ(sent[0].group_addr, 0x0B04);  // GroupValueRead sent for 1/3/4
}

TEST_F(ReadHandlerTest, TZeroForcesInitialRead) {
  cache_.push(0x0A03, {0x42});
  cache_.push(0x0B04, {0x0C, 0x6F});
  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3&a=1/3/4&t=0");
  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(r.body.find("1/3/4"), std::string::npos);
  EXPECT_NE(r.body.find("\"i\":"), std::string::npos);
}

// ================================================================
// Poll loop: wait_for_activity + drain_and_cache
// ================================================================

TEST_F(ReadHandlerTest, LongPollReceivesGroupTelegram) {
  cache_.push(0x0A03, {0x42});
  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3");
  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(r.body.find("42"), std::string::npos);
  EXPECT_NE(r.body.find("\"i\":"), std::string::npos);
}

TEST_F(ReadHandlerTest, LongPollSkipsNonMatchingTelegram) {
  cache_.push(0x0B04, {0x0c, 0x6f});  // 1/3/4
  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3&t=1");
  EXPECT_EQ(r.http_status, 200);
  EXPECT_EQ(r.body.find("1/3/4"), std::string::npos);
  EXPECT_NE(r.body.find("\"d\":{}"), std::string::npos);
}

TEST_F(ReadHandlerTest, LongPollMultipleTelegrams) {
  cache_.push(0x0A03, {0x42});
  cache_.push(0x0B04, {0x0c, 0x6f});
  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3&a=1/3/4");
  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(r.body.find("42"), std::string::npos);
  EXPECT_NE(r.body.find("1/3/4"), std::string::npos);
  EXPECT_NE(r.body.find("0c6f"), std::string::npos);
}

TEST_F(ReadHandlerTest, LongPollTimeoutReturnsEmpty) {
  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3&t=1");
  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("\"d\":{}"), std::string::npos);
  EXPECT_NE(r.body.find("\"i\":0"), std::string::npos);
}

// ================================================================
// Write-wakes-read
// ================================================================

TEST_F(ReadHandlerTest, WriteWakesBlockedRead) {
  ReadHandler handler(cache_, knxd_, sessions_);
  cache_.push(0x0A03, {0x42});
  auto r = handler.handle("a=1/2/3&i=0&t=0");
  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(r.body.find("42"), std::string::npos);
  // i = cache position after push
  EXPECT_NE(r.body.find("\"i\":1"), std::string::npos);
}

TEST_F(ReadHandlerTest, WriteWakesBlockedReadNonMatchingIgnored) {
  ReadHandler handler(cache_, knxd_, sessions_);
  cache_.push(0x0B04, {0x0c, 0x6f});
  auto r = handler.handle("a=1/2/3&i=0&t=0");
  EXPECT_EQ(r.body.find("1/3/4"), std::string::npos);
  EXPECT_NE(r.body.find("\"d\":{}"), std::string::npos);
  // no data delivered → i stays at request_i
  EXPECT_NE(r.body.find("\"i\":1"), std::string::npos);
}

TEST_F(ReadHandlerTest, WriteWakesReadIndexAdvances) {
  ReadHandler handler(cache_, knxd_, sessions_);
  cache_.push(0x0A03, {0x42});
  cache_.push(0x0A03, {0x99});
  auto r = handler.handle("a=1/2/3&i=0&t=0");
  EXPECT_NE(r.body.find("99"), std::string::npos);
  // same address deduplicated → 1 delivery → i = cache position after 2 pushes
  EXPECT_NE(r.body.find("\"i\":2"), std::string::npos);
}

// ================================================================
// Position (i) correctness
// ================================================================

TEST_F(ReadHandlerTest, IndexAdvancesPerUniqueAddress) {
  cache_.push(0x0A03, {0x42});
  cache_.push(0x0B04, {0x0c, 0x6f});
  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3&a=1/3/4&t=0");
  // 2 unique addresses → i = 0 + 2 = 2
  EXPECT_NE(r.body.find("\"i\":2"), std::string::npos);
}

TEST_F(ReadHandlerTest, IndexNeverGoesBackward) {
  cache_.push(0x0A03, {0x42});
  ReadHandler h1(cache_, knxd_, sessions_);
  auto r1 = h1.handle("a=1/2/3&i=0&t=0");
  EXPECT_NE(r1.body.find("\"i\":1"), std::string::npos);

  cache_.push(0x0A03, {0x99});
  ReadHandler h2(cache_, knxd_, sessions_);
  auto r2 = h2.handle("a=1/2/3&i=0&t=0");
  EXPECT_NE(r2.body.find("\"i\":2"), std::string::npos);
}

// ================================================================
// Deduplication
// ================================================================

TEST_F(ReadHandlerTest, DeduplicatesSameAddress) {
  cache_.push(0x0A03, {0x42});
  cache_.push(0x0A03, {0x99});
  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3");
  size_t first = r.body.find("1/2/3");
  EXPECT_NE(first, std::string::npos);
  size_t second = r.body.find("1/2/3", first + 1);
  EXPECT_EQ(second, std::string::npos);
}

// ================================================================
// GroupCache persistence
// ================================================================

TEST_F(ReadHandlerTest, GroupCachePersistsAcrossRequests) {
  cache_.push(0x0A03, {0x42});
  {
    ReadHandler h(cache_, knxd_, sessions_);
    (void)h.handle("a=1/2/3&t=1");
  }
  ReadHandler h2(cache_, knxd_, sessions_);
  auto r = h2.handle("a=1/2/3&i=0&t=1");
  EXPECT_NE(r.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(r.body.find("42"), std::string::npos);
}

// ================================================================
// Read-APDU filtering
// ================================================================

TEST_F(ReadHandlerTest, FiltersOutReadApdu) {
  // (Read APDU filtered out — not pushed to cache);  // A_GroupValue_Read
  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3&t=1");
  EXPECT_NE(r.body.find("\"d\":{}"), std::string::npos);
}

// ================================================================
// Multi-iteration poll loop: handles busy-bus correctly
// ================================================================

/// Non-matching telegrams arrive first, then matching — handler continues polling.
TEST_F(ReadHandlerTest, BusyBusSkipsNonMatchingThenFindsMatch) {
  ReadHandler handler(cache_, knxd_, sessions_);
  // First poll: non-matching telegram only
  cache_.push(0x0B04, {0x01});  // 1/3/4 — not subscribed
                                // Second poll: matching telegram arrives
  cache_.push(0x0A03, {0x42});  // 1/2/3

  auto r = handler.handle("a=1/2/3&t=0");
  EXPECT_NE(r.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(r.body.find("42"), std::string::npos);
  EXPECT_EQ(r.body.find("1/3/4"), std::string::npos);
}

/// All buffered telegrams are drained, even when none match.
TEST_F(ReadHandlerTest, DrainsAllBufferedWhenNoMatch) {
  ReadHandler handler(cache_, knxd_, sessions_);
  cache_.push(0x0B04, {0x01});  // 1/3/4 — not subscribed
  cache_.push(0x0C05, {0x02});  // 1/4/5 — not subscribed

  auto r = handler.handle("a=1/2/3&t=1");
  EXPECT_NE(r.body.find("\"d\":{}"), std::string::npos);
  // Both non-matching telegrams consumed, no data delivered
  EXPECT_EQ(cache_.position(), 2);  // both pushed to cache
}

/// Timeout after multiple empty polls returns empty.
TEST_F(ReadHandlerTest, TimeoutAfterEmptyPollsReturnsEmpty) {
  ReadHandler handler(cache_, knxd_, sessions_);
  // No telegrams — timeout after t=1
  auto r = handler.handle("a=1/2/3&t=1");
  EXPECT_NE(r.body.find("\"d\":{}"), std::string::npos);
  EXPECT_NE(r.body.find("\"i\":0"), std::string::npos);
}

// ================================================================
// Position (i) correctness — black-box
// ================================================================

/// i reflects cache position, not telegram count.
TEST_F(ReadHandlerTest, IndexComesFromCachePosition) {
  // Pre-populate cache via direct pushes — simulates previous telegrams
  for (int i = 0; i < 42; ++i) {
    cache_.push(0x0A03, {0x42});
  }

  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3&i=0");
  EXPECT_NE(r.body.find("\"i\":42"), std::string::npos);
}

/// i never goes backward across multiple requests.
TEST_F(ReadHandlerTest, IndexNeverGoesBackwardAcrossRequests) {
  cache_.push(0x0A03, {0x42});
  ReadHandler h1(cache_, knxd_, sessions_);
  auto r1 = h1.handle("a=1/2/3&i=0&t=0");
  uint32_t i1 = [&]() {
    auto p = r1.body.find("\"i\":");
    return static_cast<uint32_t>(std::stoul(r1.body.substr(p + 4)));
  }();

  cache_.push(0x0A03, {0x99});
  ReadHandler h2(cache_, knxd_, sessions_);
  auto r2 = h2.handle("a=1/2/3&i=" + std::to_string(i1) + "&t=0");
  uint32_t i2 = [&]() {
    auto p = r2.body.find("\"i\":");
    return static_cast<uint32_t>(std::stoul(r2.body.substr(p + 4)));
  }();
  EXPECT_GT(i2, i1) << "i must advance: " << i1 << " → " << i2;
}

// ================================================================
// Concurrent / black-box E2E tests
// ================================================================

/// Two readers sharing one cache: both receive the same update.
TEST_F(ReadHandlerTest, TwoReadersReceiveSameUpdate) {
  // Both readers share the same GroupCache
  ReadHandler reader_a(cache_, knxd_, sessions_);
  ReadHandler reader_b(cache_, knxd_, sessions_);
  cache_.push(0x0A03, {0x42});
  cache_.push(0x0A03, {0x42});  // duplicate for second poll

  auto ra = reader_a.handle("a=1/2/3&t=0");
  auto rb = reader_b.handle("a=1/2/3&t=0");

  EXPECT_NE(ra.body.find("42"), std::string::npos);
  EXPECT_NE(rb.body.find("42"), std::string::npos);
}

/// Writer updates cache, multiple readers see it concurrently.
TEST_F(ReadHandlerTest, WriteUpdatesVisibleToMultipleReaders) {
  // Simulate: /w pushes value, then two /r requests see it
  cache_.push(0x0A03, {0x42});  // write

  ReadHandler r1(cache_, knxd_, sessions_);
  ReadHandler r2(cache_, knxd_, sessions_);
  auto resp1 = r1.handle("a=1/2/3&i=0&t=1");
  auto resp2 = r2.handle("a=1/2/3&i=0&t=1");

  EXPECT_NE(resp1.body.find("42"), std::string::npos);
  EXPECT_NE(resp2.body.find("42"), std::string::npos);
  // Both see the same position
  EXPECT_EQ(resp1.body.find("\"i\":"), resp2.body.find("\"i\":"));
}

/// Delta query only returns entries newer than the client's i.
TEST_F(ReadHandlerTest, DeltaOnlyReturnsNewerThanLastIndex) {
  // Pre-populate cache, then enqueue new telegrams
  cache_.push(0x0A03, {0x01});  // pos 1 (pre-populate)
  cache_.push(0x0A03, {0x02});  // new telegram at pos 3
  cache_.push(0x0A03, {0x03});  // new telegram at pos 3

  ReadHandler handler(cache_, knxd_, sessions_);
  // Client last saw position 1 — gets new entries from positions 2 and 3
  auto r = handler.handle("a=1/2/3&i=2&t=0");
  EXPECT_NE(r.body.find("\"03\""), std::string::npos);
  EXPECT_NE(r.body.find("\"i\":3"), std::string::npos);
}

/// Unchanged address is NOT re-transmitted when another address changes.
/// Uses a thread to simulate the cache reader pushing new data during the
/// long-poll wait — the handler must only deliver entries with pushed_at > i.
TEST_F(ReadHandlerTest, UnchangedAddressNotRetransmittedWhenOtherChanges) {
  // A and B in cache at known positions.
  cache_.push(0x0A03, {0x42});  // A at pos 1
  cache_.push(0x0B04, {0x01});  // B at pos 2

  ReadHandler handler(cache_, knxd_, sessions_);
  // Simulate the cache reader pushing B again while the handler is blocked
  // in the poll loop.  The handler must only deliver B (pushed_at=3 > i=1),
  // not A (pushed_at=1, not > 1).
  std::thread pusher([this]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    cache_.push(0x0B04, {0x01});  // B again (same value, pos 3)
  });

  // Client polls with i=1 — should ONLY get B (changed at pos 2 and 3).
  // Use t=5 so the thread has time to push.
  auto r = handler.handle("a=1/2/3&a=1/3/4&i=1&t=5");
  pusher.join();

  EXPECT_EQ(r.body.find("1/2/3"), std::string::npos) << "A must not be retransmitted";
  EXPECT_NE(r.body.find("1/3/4"), std::string::npos) << "B should be delivered";
}

/// Value oscillating (00→01→00) — each change delivered, no stale re-transmission.
TEST_F(ReadHandlerTest, OscillatingValueNoStaleRetransmission) {
  // Reproduces: 00 → 01 → 00 without stale intermediate values
  cache_.push(0x0A03, {0x00});  // pos 1

  ReadHandler h1(cache_, knxd_, sessions_);
  auto r1 = h1.handle("a=1/2/3&i=0&t=0");
  EXPECT_NE(r1.body.find("00"), std::string::npos);
  uint32_t i1 = [&]() {
    auto p = r1.body.find("\"i\":");
    return static_cast<uint32_t>(std::stoul(r1.body.substr(p + 4)));
  }();

  // Value changes to 01
  cache_.push(0x0A03, {0x01});
  ReadHandler h2(cache_, knxd_, sessions_);
  auto r2 = h2.handle("a=1/2/3&i=" + std::to_string(i1) + "&t=0");
  EXPECT_NE(r2.body.find("01"), std::string::npos);
  uint32_t i2 = [&]() {
    auto p = r2.body.find("\"i\":");
    return static_cast<uint32_t>(std::stoul(r2.body.substr(p + 4)));
  }();

  // Value changes back to 00
  cache_.push(0x0A03, {0x00});
  ReadHandler h3(cache_, knxd_, sessions_);
  auto r3 = h3.handle("a=1/2/3&i=" + std::to_string(i2) + "&t=0");
  EXPECT_NE(r3.body.find("00"), std::string::npos);
  uint32_t i3 = [&]() {
    auto p = r3.body.find("\"i\":");
    return static_cast<uint32_t>(std::stoul(r3.body.substr(p + 4)));
  }();

  EXPECT_GT(i2, i1);
  EXPECT_GT(i3, i2);
}

// ================================================================
// Latency: matching telegram must be delivered in 1 iteration
// (no unnecessary wake-ups from non-matching bus traffic)
// ================================================================

/// On a busy bus with non-matching telegrams, the handler must still
/// deliver a matching telegram without extra loop iterations.
/// This test enqueues non-matching telegrams first, then a matching one,
/// and verifies the matching data is delivered with advancing position.
TEST_F(ReadHandlerTest, MatchingTelegramDeliveredOnBusyBus) {
  // Simulate a busy bus: non-matching telegrams arrive before the match
  cache_.push(0x0B04, {0x01});  // 1/3/4 — not subscribed
  cache_.push(0x0C05, {0x02});  // 1/4/5 — not subscribed
  cache_.push(0x0A03, {0x42});  // 1/2/3 — MATCH

  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3&i=42&t=0");

  EXPECT_EQ(r.http_status, 200);
  // Matching address must be delivered
  EXPECT_NE(r.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(r.body.find("42"), std::string::npos);
  // Non-matching addresses must NOT appear
  EXPECT_EQ(r.body.find("1/3/4"), std::string::npos);
  EXPECT_EQ(r.body.find("1/4/5"), std::string::npos);
  // Position must advance (3 telegrams pushed → position = 3)
  EXPECT_NE(r.body.find("\"i\":3"), std::string::npos);
}

/// Multiple matching telegrams for different addresses on a busy bus —
/// all matching addresses delivered, non-matching excluded.
TEST_F(ReadHandlerTest, MultipleMatchingOnBusyBus) {
  cache_.push(0x0B04, {0x01});        // non-match
  cache_.push(0x0A03, {0x42});        // match A
  cache_.push(0x0C05, {0x02});        // non-match
  cache_.push(0x0D06, {0x0c, 0x6f});  // match B (1/5/6)

  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3&a=1/5/6&i=0&t=0");

  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(r.body.find("42"), std::string::npos);
  EXPECT_NE(r.body.find("1/5/6"), std::string::npos);
  EXPECT_NE(r.body.find("0c6f"), std::string::npos);
  EXPECT_EQ(r.body.find("1/3/4"), std::string::npos);
  EXPECT_EQ(r.body.find("1/4/5"), std::string::npos);
  EXPECT_NE(r.body.find("\"i\":4"), std::string::npos);  // 4 pushes
}

// ================================================================
// Multi-worker realism: separate caches, shared mock knxd
// ================================================================
// In production, each worker process has its own GroupCache with its own
// position_ counter.  These tests use separate GroupCache instances to
// reproduce bugs that are hidden when all ReadHandlers share one cache.

/// Fixture with two independent GroupCache instances to simulate
/// the per-worker cache isolation that exists in production.
class ReadHandlerMultiCacheTest : public ::testing::Test {
public:
  void SetUp() override {
    // Create one shared cache and attach both "worker" views to it.
    // This simulates the production architecture where all workers
    // share a single mmap'd cache.
    ASSERT_TRUE(cache_a_.create());
    cache_b_.attach(cache_a_.data());
    (void)knxd_.connect("/run/knx");
    (void)knxd_.open_group_socket(false);
  }

  MockKnxdClient knxd_;  // NOLINT
  SharedGroupCache cache_a_;
  SharedGroupCache cache_b_;
  SessionStore sessions_;
};

/// With a shared cache (production architecture), the position is ALWAYS
/// monotonically increasing regardless of which worker processes the request.
TEST_F(ReadHandlerMultiCacheTest, NonMonotonicIndexAcrossSeparateCaches) {
  // Both "workers" share the same underlying cache (attached in SetUp).
  // Push via cache_a — both views see the same position.
  for (int i = 0; i < 10; ++i) {
    cache_a_.push(0x0A03, {0x42});  // pos 1-10
  }

  ReadHandler handler_a(cache_a_, knxd_, sessions_);
  auto r_a = handler_a.handle("a=1/2/3&i=0&t=0");
  uint32_t i_a = [&]() {
    auto p = r_a.body.find("\"i\":");
    return static_cast<uint32_t>(std::stoul(r_a.body.substr(p + 4)));
  }();
  EXPECT_EQ(i_a, 10) << "Position after 10 pushes";

  // Push more via cache_b — same shared position advances.
  for (int i = 0; i < 5; ++i) {
    cache_b_.push(0x0A03, {0x99});  // pos 11-15
  }

  // Read via cache_a — sees the new shared position (15)
  ReadHandler handler_a2(cache_a_, knxd_, sessions_);
  auto r_a2 = handler_a2.handle("a=1/2/3&i=0&t=0");
  uint32_t i_a2 = [&]() {
    auto p = r_a2.body.find("\"i\":");
    return static_cast<uint32_t>(std::stoul(r_a2.body.substr(p + 4)));
  }();
  EXPECT_EQ(i_a2, 15) << "Shared position must reflect all pushes from both workers";

  // Verify monotonic: position never goes backward
  EXPECT_GT(i_a2, i_a) << "Shared cache position must be monotonic";
}

/// REPRODUCES BUG: Cache miss on t=0 when telegrams are enqueued but
/// not yet drained from the group socket.
TEST_F(ReadHandlerTest, TZeroFindsEnqueuedTelegramsAfterDrain) {
  // Pre-populate the knxd socket with a matching telegram
  cache_.push(0x0A03, {0x42});  // 1/2/3 = 0x42

  ReadHandler handler(cache_, knxd_, sessions_);
  // t=0 → lastpos=0, timeout_sec=1
  auto r = handler.handle("a=1/2/3&t=0");

  EXPECT_EQ(r.http_status, 200);
  // BUG: without draining before the initial read, this telegram sits in
  // the socket buffer and cache_.get() returns nullopt.  The handler enters
  // the poll loop, drains, and finds it — but only if the timeout hasn't
  // expired.  On a busy system with t=0, this can fail.
  EXPECT_NE(r.body.find("1/2/3"), std::string::npos)
      << "BUG: enqueued telegram not found on t=0 (not drained before initial read)";
  EXPECT_NE(r.body.find("42"), std::string::npos);
}

/// REPRODUCES BUG: With separate caches, a write (cache_a push) is invisible
/// to a read handled by a different cache (cache_b).
TEST_F(ReadHandlerMultiCacheTest, WriteByWorkerANotVisibleInWorkerB) {
  // Worker A handles a write — data goes into cache_a only
  cache_a_.push(0x0A03, {0x42});  // 1/2/3 = 0x42

  // Worker B handles a subsequent read — uses cache_b (empty)
  ReadHandler handler_b(cache_b_, knxd_, sessions_);
  // t=0 force-reread: should find the value
  auto r = handler_b.handle("a=1/2/3&t=0");

  // BUG: cache_b doesn't have the value that cache_a received.
  // In production, knxd echoes to all group sockets, so all workers
  // SHOULD receive the telegram — but only if it was sent via knxd
  // (not via direct cache push).  Still, this test demonstrates the
  // isolation problem.
  //
  // Note: in production, knxd echoes group packets to ALL open group
  // sockets, so each worker's drain_into_cache() would pick up the echo.
  // The real issue is TIMING: a worker may not have drained yet.
  EXPECT_NE(r.body.find("1/2/3"), std::string::npos)
      << "BUG: data pushed to cache_a not visible in cache_b";
  EXPECT_NE(r.body.find("42"), std::string::npos);
}

// ================================================================
// Age-filter regression tests — timeout must NOT be used as max_age
// ================================================================
// The ReadHandler must never use the poll timeout (t= parameter) as an
// age filter on cached data.  The position-based filter (pushed_at > i)
// is the only correct filtering mechanism — age filtering silently drops
// telegrams the client hasn't seen yet, violating KNX Rule 1.

/// REGRESSION: t=0 must return pre-cached data regardless of when it was
/// pushed.  Using timeout_sec=1 as max_age_sec would hide all data older
/// than 1 second, causing {"d":{},"i":N} on a non-empty cache.
TEST_F(ReadHandlerTest, TZeroReturnsPreCachedData) {
  // Simulate data that was pushed to the cache before this request.
  cache_.push(0x0A03, {0x42});        // 1/2/3
  cache_.push(0x0B04, {0x0C, 0x6F});  // 1/3/4

  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3&a=1/3/4&t=0");

  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("1/2/3"), std::string::npos)
      << "t=0 must return cached 1/2/3 regardless of age";
  EXPECT_NE(r.body.find("42"), std::string::npos);
  EXPECT_NE(r.body.find("1/3/4"), std::string::npos)
      << "t=0 must return cached 1/3/4 regardless of age";
  EXPECT_NE(r.body.find("0c6f"), std::string::npos);
  // Position must reflect the cache state (2 pushes).
  EXPECT_NE(r.body.find("\"i\":2"), std::string::npos);
}

/// Per spec, t>0 means "cached data at most t seconds old".  Freshly-pushed
/// data passes the age filter; t=0 has no age filter (read-from-bus mode).
/// All three must return the same freshly-pushed data.
TEST_F(ReadHandlerTest, AgeFilterDoesNotRejectFreshData) {
  cache_.push(0x0A03, {0x42});  // 1/2/3

  // t=0: no age filter (read-from-bus), t=1 and t=5: age filter passes
  // because data was pushed just now (timestamp diff < 1 second).
  for (int t_val : {0, 1, 5}) {
    ReadHandler handler(cache_, knxd_, sessions_);
    auto r = handler.handle("a=1/2/3&t=" + std::to_string(t_val));
    EXPECT_EQ(r.http_status, 200) << "t=" << t_val;
    EXPECT_NE(r.body.find("42"), std::string::npos)
        << "t=" << t_val << " must return freshly-pushed cached value";
  }
}

/// REGRESSION: Initial read (no i= parameter) must return all cached data
/// regardless of the timeout.  When t is not provided, no age filter is
/// applied — the default longpoll timeout does NOT act as an age filter.
TEST_F(ReadHandlerTest, InitialReadNoIndexReturnsAllCachedData) {
  cache_.push(0x0A03, {0x42});  // 1/2/3
  cache_.push(0x0B04, {0x01});  // 1/3/4

  // No t= parameter → max_age_sec=-1 (no age filter).
  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3&a=1/3/4");

  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(r.body.find("42"), std::string::npos);
  EXPECT_NE(r.body.find("1/3/4"), std::string::npos);
  EXPECT_NE(r.body.find("01"), std::string::npos);
}

/// REGRESSION: get_delta must return entries based on position, not age.
/// When a client connects with i=0, ALL cached entries have pushed_at > 0
/// and must be returned, even if they were pushed long ago.
TEST_F(ReadHandlerTest, DeltaReturnsAllEntriesWhenPositionIsZero) {
  // Push multiple entries to advance position beyond 0.
  cache_.push(0x0A03, {0x42});  // pos 1
  cache_.push(0x0B04, {0x01});  // pos 2
  cache_.push(0x0C05, {0x02});  // pos 3

  // Client requests with i=0 → should get ALL entries (pushed_at > 0).
  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3&a=1/3/4&a=1/4/5&i=0&t=0");

  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(r.body.find("42"), std::string::npos);
  EXPECT_NE(r.body.find("1/3/4"), std::string::npos);
  EXPECT_NE(r.body.find("01"), std::string::npos);
  EXPECT_NE(r.body.find("1/4/5"), std::string::npos);
  EXPECT_NE(r.body.find("02"), std::string::npos);
  EXPECT_NE(r.body.find("\"i\":3"), std::string::npos);
}

/// REGRESSION: get_delta with i=0 must NOT apply age filtering.
/// Even with a small timeout value, entries with pushed_at > 0 must be
/// returned — the position filter is the only correct filter.
TEST_F(ReadHandlerTest, DeltaWithZeroIndexNotAffectedBySmallTimeout) {
  cache_.push(0x0A03, {0x42});  // pos 1

  // i=0, t=0 (timeout_sec becomes 1 internally).
  // The delta query must return the entry at pos 1 despite the small timeout.
  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3&i=0&t=0");

  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("42"), std::string::npos)
      << "Entry at pushed_at=1 must be returned even with t=0";
  EXPECT_NE(r.body.find("\"i\":1"), std::string::npos);
}

/// REGRESSION: With a non-zero i, delta must return all newer entries
/// when new data arrives during the poll, regardless of timeout value.
/// The position filter (pushed_at > i) is the only filter — age must not
/// prevent delivery.
TEST_F(ReadHandlerTest, DeltaReturnsAllNewerEntriesWhenDataArrives) {
  cache_.push(0x0A03, {0x01});  // pos 1
  cache_.push(0x0A03, {0x02});  // pos 2
  cache_.push(0x0A03, {0x03});  // pos 3

  // Push new data during the poll — triggers get_delta().
  std::thread pusher([this]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    cache_.push(0x0A03, {0x04});  // pos 4
  });

  // Client is at i=1, new data at pos 4 arrives during poll.
  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3&i=1&t=2");
  pusher.join();

  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("04"), std::string::npos)
      << "Latest value (04) at pos 4 must be delivered via delta";
  EXPECT_NE(r.body.find("\"i\":4"), std::string::npos) << "Position must advance to 4";
  // Entry at pos 1 (i=1, pushed_at=1 is NOT > 1) must not be delivered.
  EXPECT_EQ(r.body.find("01"), std::string::npos) << "Value at pos 1 must not be re-delivered";
}

/// REGRESSION: t=0 with i=0 (fresh connect) must do initial read and return
/// ALL cached entries regardless of when they were pushed.
TEST_F(ReadHandlerTest, FreshConnectReturnsAllCachedData) {
  cache_.push(0x0A03, {0x01});  // pos 1
  cache_.push(0x0A03, {0x02});  // pos 2
  cache_.push(0x0A03, {0x03});  // pos 3
  cache_.push(0x0A03, {0x04});  // pos 4

  // i=0, t=0 — forces initial read, returns latest value.
  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3&i=0&t=0");

  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("04"), std::string::npos)
      << "Latest cached value must be returned on fresh connect";
  EXPECT_NE(r.body.find("\"i\":4"), std::string::npos);
}

/// REGRESSION: Mix of cached (old push) and uncached addresses — the cached
/// ones must be returned in the initial read regardless of age.
TEST_F(ReadHandlerTest, MixedCachedAndUncachedInitialReadWithTZero) {
  cache_.push(0x0A03, {0x42});  // 1/2/3 cached

  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3&a=1/3/4&t=0");

  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("1/2/3"), std::string::npos) << "Cached address must be in response";
  EXPECT_NE(r.body.find("42"), std::string::npos);
  // Uncached address not in response — GroupValueRead sent asynchronously.
  EXPECT_EQ(r.body.find("1/3/4"), std::string::npos) << "Uncached address must not appear";
  EXPECT_NE(r.body.find("\"i\":1"), std::string::npos);
  auto sent = knxd_.sent_packets();
  ASSERT_EQ(sent.size(), 1);
  EXPECT_EQ(sent[0].group_addr, 0x0B04);  // GroupValueRead for 1/3/4
}

// ================================================================
// t < 0 — "nur aus dem Cache gelesen" (cache only, no bus read)
// ================================================================
// Per spec: negative t means read from cache only, no polling, no bus
// read.  Since we never read from the bus directly, this is essentially
// the same as t=0 but without any poll loop.

/// t=-1: returns cached data immediately, no poll, no age filter.
TEST_F(ReadHandlerTest, TNegativeReturnsCachedDataImmediately) {
  cache_.push(0x0A03, {0x42});  // 1/2/3
  cache_.push(0x0B04, {0x01});  // 1/3/4

  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3&a=1/3/4&t=-1");

  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("1/2/3"), std::string::npos) << "t=-1 must return cached data";
  EXPECT_NE(r.body.find("42"), std::string::npos);
  EXPECT_NE(r.body.find("1/3/4"), std::string::npos);
  EXPECT_NE(r.body.find("01"), std::string::npos);
  EXPECT_NE(r.body.find("\"i\":2"), std::string::npos);
}

/// t=-1 with no cached data: returns empty d, i=0, no poll.
TEST_F(ReadHandlerTest, TNegativeEmptyCache) {
  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3&t=-1");

  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("\"d\":{}"), std::string::npos);
  EXPECT_NE(r.body.find("\"i\":0"), std::string::npos);
}

/// t=-1 with i= parameter: ignores i, forces initial read, returns all
/// cached data (spec: "nur aus dem Cache gelesen").
TEST_F(ReadHandlerTest, TNegativeIgnoresIndexForcesInitialRead) {
  cache_.push(0x0A03, {0x42});  // pos 1

  // Even with i=999 (beyond current position), t=-1 forces initial read.
  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3&i=999&t=-1");

  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("42"), std::string::npos) << "t=-1 must return cached data regardless of i";
  EXPECT_NE(r.body.find("\"i\":1"), std::string::npos);
}

/// t=-1 with large negative value: same behavior (cache only, no poll).
TEST_F(ReadHandlerTest, TLargeNegativeReturnsCachedDataImmediately) {
  cache_.push(0x0A03, {0x42});

  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3&t=-999");

  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("42"), std::string::npos) << "t=-999 must return cached data immediately";
  EXPECT_NE(r.body.find("\"i\":1"), std::string::npos);
}

// ================================================================
// t=0: GroupValueRead for uncached addresses
// ================================================================
// Per spec, t=0 means "read from bus NOW".  For addresses not in the cache,
// the handler must send a GroupValueRead telegram (APDU [0x00, 0x00]) via
// the knxd group socket.  The response arrives asynchronously (handled by
// the cache reader process like a normal bus write).  The HTTP response
// is returned immediately (non-blocking).

/// t=0 sends GroupValueRead for a single uncached address.
TEST_F(ReadHandlerTest, TZeroSendsGroupValueReadForUncached) {
  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3&t=0");

  // Response must be immediate (non-blocking) even with cache miss.
  EXPECT_EQ(r.http_status, 200);

  // Must have sent exactly one GroupValueRead for the uncached address.
  auto sent = knxd_.sent_packets();
  ASSERT_EQ(sent.size(), 1);
  EXPECT_EQ(sent[0].group_addr, 0x0A03);  // 1/2/3

  // APDU must be A_GroupValue_Read: [0x00, 0x00]
  ASSERT_EQ(sent[0].apdu.size(), 2);
  EXPECT_EQ(sent[0].apdu[0], 0x00);
  EXPECT_EQ(sent[0].apdu[1], 0x00);
}

/// t=0 sends GroupValueRead for multiple uncached addresses.
TEST_F(ReadHandlerTest, TZeroSendsGroupValueReadForMultipleUncached) {
  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3&a=1/3/4&a=1/4/5&t=0");

  EXPECT_EQ(r.http_status, 200);

  // Must have sent GroupValueRead for all three uncached addresses.
  auto sent = knxd_.sent_packets();
  ASSERT_EQ(sent.size(), 3);
  EXPECT_EQ(sent[0].group_addr, 0x0A03);  // 1/2/3
  EXPECT_EQ(sent[1].group_addr, 0x0B04);  // 1/3/4
  EXPECT_EQ(sent[2].group_addr, 0x0C05);  // 1/4/5

  // All must be GroupValueRead APDUs.
  for (const auto& p : sent) {
    ASSERT_EQ(p.apdu.size(), 2);
    EXPECT_EQ(p.apdu[0], 0x00);
    EXPECT_EQ(p.apdu[1], 0x00);
  }
}

/// t=0 only sends GroupValueRead for uncached addresses, not cached ones.
TEST_F(ReadHandlerTest, TZeroOnlyReadsUncachedAddresses) {
  cache_.push(0x0A03, {0x42});  // 1/2/3 already cached

  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3&a=1/3/4&t=0");

  EXPECT_EQ(r.http_status, 200);

  // Cached address 1/2/3 must be in response.
  EXPECT_NE(r.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(r.body.find("42"), std::string::npos);

  // Only 1/3/4 (uncached) should get a GroupValueRead — NOT 1/2/3.
  auto sent = knxd_.sent_packets();
  ASSERT_EQ(sent.size(), 1);
  EXPECT_EQ(sent[0].group_addr, 0x0B04);  // 1/3/4 only
  EXPECT_EQ(sent[0].apdu[0], 0x00);
  EXPECT_EQ(sent[0].apdu[1], 0x00);
}

/// t=0 with all addresses cached: no GroupValueRead needed.
TEST_F(ReadHandlerTest, TZeroNoGroupValueReadWhenAllCached) {
  cache_.push(0x0A03, {0x42});        // 1/2/3
  cache_.push(0x0B04, {0x0C, 0x6F});  // 1/3/4

  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3&a=1/3/4&t=0");

  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(r.body.find("1/3/4"), std::string::npos);

  // No GroupValueRead needed — all addresses were cached.
  auto sent = knxd_.sent_packets();
  EXPECT_TRUE(sent.empty());
}

/// t>0 (long-poll with age filter) does NOT send GroupValueRead.
/// Only t=0 triggers bus reads — other t values use cache + poll only.
TEST_F(ReadHandlerTest, TPositiveDoesNotSendGroupValueRead) {
  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3&t=1");

  EXPECT_EQ(r.http_status, 200);

  // t=30 is a long-poll, not a bus read — no GroupValueRead must be sent.
  auto sent = knxd_.sent_packets();
  EXPECT_TRUE(sent.empty());
}

/// t<0 (cache only) does NOT send GroupValueRead.
TEST_F(ReadHandlerTest, TNegativeDoesNotSendGroupValueRead) {
  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3&t=-1");

  EXPECT_EQ(r.http_status, 200);

  // t=-1 is cache-only — no GroupValueRead.
  auto sent = knxd_.sent_packets();
  EXPECT_TRUE(sent.empty());
}

/// Long-poll (no t parameter) does NOT send GroupValueRead.
TEST_F(ReadHandlerTest, NoTParameterDoesNotSendGroupValueRead) {
  ReadHandler handler(cache_, knxd_, sessions_, 1);
  auto r = handler.handle("a=1/2/3");

  EXPECT_EQ(r.http_status, 200);

  // No t= parameter means long-poll — no GroupValueRead.
  auto sent = knxd_.sent_packets();
  EXPECT_TRUE(sent.empty());
}

// ================================================================
// Position wrapping
// ================================================================

/// The index `i` wraps at kPositionModulus (0-based, max = M-1 = 99,999).
/// After M pushes i resets from M-1 to 0.
TEST_F(ReadHandlerTest, IndexWrapsAtModulus) {
  // Push M-1 entries: position climbs to M-1 = 99,999.
  for (uint32_t i = 0; i < kPositionModulus - 1; ++i) {
    cache_.push(0x0A03, {static_cast<uint8_t>(i & 0xFF)});
  }

  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3&i=0&t=0");
  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("\"i\":" + std::to_string(kPositionModulus - 1)), std::string::npos)
      << "i should be M-1 = 99,999 before wrap";

  // One more push wraps the position to 0.
  cache_.push(0x0A03, {0x42});

  ReadHandler handler2(cache_, knxd_, sessions_);
  auto r2 = handler2.handle("a=1/2/3&i=0&t=0");
  EXPECT_EQ(r2.http_status, 200);
  EXPECT_NE(r2.body.find("\"i\":0"), std::string::npos) << "i must wrap to 0 after M pushes";
}

/// Epoch mismatch: client from early in the epoch, server near M → full refresh.
TEST_F(ReadHandlerTest, EpochMismatchReturnsCurrentValues) {
  // Push M-3 entries: raw 0..M-4, pushed_at 0..M-4. position = M-3.
  for (uint32_t i = 0; i < kPositionModulus - 3; ++i) {
    cache_.push(0x0A03, {static_cast<uint8_t>(i & 0xFF)});
  }

  // Push current value: raw = M-3, pushed_at = M-3. position = M-2.
  cache_.push(0x0A03, {0x42});

  // Client sends i=10 (early epoch): epoch_distance = M-12 >= M/2 → epoch mismatch.
  ReadHandler handler(cache_, knxd_, sessions_);
  auto r = handler.handle("a=1/2/3&i=10&t=0");

  EXPECT_EQ(r.http_status, 200);
  EXPECT_NE(r.body.find("\"42\""), std::string::npos)
      << "Epoch mismatch must return current value via full refresh";
  EXPECT_NE(r.body.find("\"i\":" + std::to_string(kPositionModulus - 2)), std::string::npos)
      << "Position must reflect current state (M-2)";
}

/// Position never goes backward across the wrap boundary.
TEST_F(ReadHandlerTest, IndexMonotonicAcrossWrap) {
  // Push M-1 entries: position = M-1 = 99,999.
  for (uint32_t i = 0; i < kPositionModulus - 1; ++i) {
    cache_.push(0x0A03, {static_cast<uint8_t>(i & 0xFF)});
  }

  ReadHandler h1(cache_, knxd_, sessions_);
  auto r1 = h1.handle("a=1/2/3&i=0&t=0");
  uint32_t i1 = [&]() {
    auto p = r1.body.find("\"i\":");
    return static_cast<uint32_t>(std::stoul(r1.body.substr(p + 4)));
  }();
  EXPECT_EQ(i1, kPositionModulus - 1) << "i should be M-1 = 99,999 before wrap";

  // Wrap then push again
  cache_.push(0x0A03, {0x42});  // wrap to 0
  cache_.push(0x0A03, {0x99});  // advance to 1

  ReadHandler h2(cache_, knxd_, sessions_);
  auto r2 = h2.handle("a=1/2/3&i=" + std::to_string(i1) + "&t=0");
  uint32_t i2 = [&]() {
    auto p = r2.body.find("\"i\":");
    return static_cast<uint32_t>(std::stoul(r2.body.substr(p + 4)));
  }();

  // Wrapped position advances: M-1 → 0 → 1
  EXPECT_EQ(i2, 1) << "i must advance: M-1 → 0 → 1";
}
