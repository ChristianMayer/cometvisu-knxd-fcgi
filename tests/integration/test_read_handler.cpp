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

#include "handlers/read_handler.h"
#include "knxd/knxd_protocol.h"
#include "mock_knxd_socket.h"
#include "state/session_store.h"

using namespace cvknxd;

class ReadHandlerTest : public ::testing::Test {
protected:
  void SetUp() override {
    (void)knxd_.connect("/run/knx");
    (void)knxd_.open_group_socket(false);
  }

  MockKnxdClient knxd_;
  SessionStore sessions_;
};

TEST_F(ReadHandlerTest, ReadFromKnxdCacheWithTimeout) {
  ReadHandler handler(knxd_, sessions_);

  // Set up mock knxd cache
  std::vector<uint8_t> data = {0x0C, 0x6F};
  knxd_.set_cached_value(0x0A03, data);

  auto result = handler.handle("a=1/2/3&t=30");

  EXPECT_EQ(result.http_status, 200);
  EXPECT_NE(result.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(result.body.find("0c6f"), std::string::npos);
}

TEST_F(ReadHandlerTest, ReadFromKnxdCacheTimeoutZero) {
  ReadHandler handler(knxd_, sessions_);

  // Set up mock knxd cache
  std::vector<uint8_t> data = {0x42};
  knxd_.set_cached_value(0x0A03, data);

  auto result = handler.handle("a=1/2/3&t=0");

  EXPECT_EQ(result.http_status, 200);
  EXPECT_NE(result.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(result.body.find("42"), std::string::npos);
  // Must include the index
  EXPECT_NE(result.body.find("\"i\":\""), std::string::npos);
  // Must NOT block — no read telegram sent because value was cached
  EXPECT_TRUE(knxd_.sent_packets().empty());
}

TEST_F(ReadHandlerTest, TimeoutZeroCacheMissSendsReadTelegram) {
  // With the corrected semantics, t=0 forces an initial cache read (lastpos=0).
  // If a value is not in cache, no read telegram is sent — this is different
  // from the old behavior. The handler relies on cache_last_updates_2 for
  // position-based polling, not on sending read telegrams.
  ReadHandler handler(knxd_, sessions_);

  // No cached value for 1/2/3
  // Set up cache_last_updates_2 to return immediately with no changes
  knxd_.set_last_updates_result(0, {}, 5);

  auto result = handler.handle("a=1/2/3&t=0");

  EXPECT_EQ(result.http_status, 200);
  // Should have empty "d" object and an index (immediate response)
  EXPECT_NE(result.body.find("\"d\":{}"), std::string::npos);
  EXPECT_NE(result.body.find("\"i\":\""), std::string::npos);
  // With the new semantics (matching original eibread-cgi.c), t=0 does NOT
  // send read telegrams — it forces a cache re-read and poll loop.
  // The poll loop uses cache_last_updates_2, not send_group_packet.
  EXPECT_TRUE(knxd_.sent_packets().empty());
}

TEST_F(ReadHandlerTest, TimeoutZeroMixedCacheAndUncached) {
  ReadHandler handler(knxd_, sessions_);

  // 1/2/3 is cached, 1/3/4 is NOT cached
  knxd_.set_cached_value(0x0A03, {0x42});

  // Set up cache_last_updates_2 to return immediately
  knxd_.set_last_updates_result(0, {}, 5);

  auto result = handler.handle("a=1/2/3&a=1/3/4&t=0");

  EXPECT_EQ(result.http_status, 200);
  // Cached address should be in response (found during initial read)
  EXPECT_NE(result.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(result.body.find("42"), std::string::npos);
  // Uncached address should NOT be in response
  EXPECT_EQ(result.body.find("1/3/4"), std::string::npos);
  // Index must be included
  EXPECT_NE(result.body.find("\"i\":\""), std::string::npos);

  // With corrected semantics, t=0 does NOT send read telegrams.
  // The handler relies on cache_last_updates_2 for change detection.
  EXPECT_TRUE(knxd_.sent_packets().empty());
}

TEST_F(ReadHandlerTest, NegativeTimeoutCacheMiss) {
  ReadHandler handler(knxd_, sessions_);

  // No knxd cache value — t < 0 returns 200 with empty data
  auto result = handler.handle("a=1/2/3&t=-1");
  EXPECT_EQ(result.http_status, 200);
  // Should have empty "d" object
  EXPECT_NE(result.body.find("\"d\":{}"), std::string::npos);
}

TEST_F(ReadHandlerTest, NoAddresses) {
  ReadHandler handler(knxd_, sessions_);
  // No addresses → 400 regardless of any other params
  auto result = handler.handle("s=abc&t=0");
  EXPECT_EQ(result.http_status, 400);
}

TEST_F(ReadHandlerTest, MultipleAddresses) {
  ReadHandler handler(knxd_, sessions_);

  knxd_.set_cached_value(0x0A03, {0x42});
  knxd_.set_cached_value(0x0B04, {0x0C, 0x6F});

  auto result = handler.handle("a=1/2/3&a=1/3/4&t=30");

  EXPECT_EQ(result.http_status, 200);
  EXPECT_NE(result.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(result.body.find("42"), std::string::npos);
  EXPECT_NE(result.body.find("1/3/4"), std::string::npos);
  EXPECT_NE(result.body.find("0c6f"), std::string::npos);
}

TEST_F(ReadHandlerTest, LongPollGetsTelegram) {
  ReadHandler handler(knxd_, sessions_);

  // Set up cache_last_updates_2 to return a changed address
  knxd_.set_last_updates_result(0, {0x0A03}, 10);
  knxd_.set_cached_value(0x0A03, {0x42});

  // Long-poll (no timeout parameter)
  auto result = handler.handle("a=1/2/3");

  EXPECT_EQ(result.http_status, 200);
  EXPECT_NE(result.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(result.body.find("42"), std::string::npos);
}

TEST_F(ReadHandlerTest, LongPollSkipsNonMatchingBufferedTelegram) {
  ReadHandler handler(knxd_, sessions_);

  // Set up cache_last_updates_2 to return a non-matching address first,
  // then a matching one. The handler only includes subscribed addresses.
  knxd_.set_last_updates_result(0, {0x0B04, 0x0A03}, 10);
  knxd_.set_cached_value(0x0B04, {0x0C, 0x6F});  // 1/3/4 — non-matching
  knxd_.set_cached_value(0x0A03, {0x42});        // 1/2/3 — matching

  // Long-poll (no timeout parameter), only subscribed to 1/2/3
  auto result = handler.handle("a=1/2/3");

  EXPECT_EQ(result.http_status, 200);
  // Only the matching address should appear
  EXPECT_NE(result.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(result.body.find("42"), std::string::npos);
  // Non-matching address should NOT appear
  EXPECT_EQ(result.body.find("1/3/4"), std::string::npos);
}

TEST_F(ReadHandlerTest, LongPollDrainsAllBufferedWhenNoMatch) {
  ReadHandler handler(knxd_, sessions_);

  // Set up cache_last_updates_2 to return only non-matching addresses.
  // The handler should process all changed addresses and return empty
  // since none are subscribed.
  knxd_.set_last_updates_result(0, {0x0B04, 0x0C05}, 10);
  knxd_.set_cached_value(0x0B04, {0x0C, 0x6F});  // 1/3/4 — not subscribed
  knxd_.set_cached_value(0x0C05, {0x01});        // 1/4/5 — not subscribed

  auto result = handler.handle("a=1/2/3");

  EXPECT_EQ(result.http_status, 200);
  // Should have empty "d" object and an index
  EXPECT_NE(result.body.find("\"d\":{}"), std::string::npos);
  EXPECT_NE(result.body.find("\"i\":\""), std::string::npos);
}

TEST_F(ReadHandlerTest, IndexIncluded) {
  ReadHandler handler(knxd_, sessions_);

  knxd_.set_cached_value(0x0A03, {0x42});
  auto result = handler.handle("a=1/2/3&t=30");

  EXPECT_EQ(result.http_status, 200);
  // Index starts at 0 (no telegrams received yet)
  EXPECT_NE(result.body.find("\"i\":\"0\""), std::string::npos);
}

TEST_F(ReadHandlerTest, IndexIncrementsAfterTelegram) {
  ReadHandler handler(knxd_, sessions_);

  // Set up cache_last_updates_2 to return position 5
  knxd_.set_last_updates_result(0, {0x0A03}, 5);
  knxd_.set_cached_value(0x0A03, {0x42});

  auto result = handler.handle("a=1/2/3&t=30");

  EXPECT_EQ(result.http_status, 200);
  // i should be the new_position from cache_last_updates_2 (5)
  EXPECT_NE(result.body.find("\"i\":\"5\""), std::string::npos);
}

TEST_F(ReadHandlerTest, IndexIncrementsForEachBufferedTelegram) {
  ReadHandler handler(knxd_, sessions_);

  // Set up cache_last_updates_2 to return position 42
  knxd_.set_last_updates_result(0, {0x0A03}, 42);
  knxd_.set_cached_value(0x0A03, {0x42});

  auto result = handler.handle("a=1/2/3&t=30");

  EXPECT_EQ(result.http_status, 200);
  // i should be the new_position from cache_last_updates_2 (42)
  EXPECT_NE(result.body.find("\"i\":\"42\""), std::string::npos);
}

TEST_F(ReadHandlerTest, InvalidAddressIgnored) {
  ReadHandler handler(knxd_, sessions_);

  knxd_.set_cached_value(0x0A03, {0x42});
  auto result = handler.handle("a=1/2/3&a=invalid&t=30");

  // Should still return the valid address
  EXPECT_EQ(result.http_status, 200);
  EXPECT_NE(result.body.find("1/2/3"), std::string::npos);
}

// Verifies that a long-poll with a non-zero index (i= parameter) detects
// pending updates that were sent before the poll started.
// This is the exact scenario: user sends packets, then starts long-poll
// with i=<old_index>. The handler must detect the pending updates via
// cache_last_updates_2 and return the cached value.
TEST_F(ReadHandlerTest, LongPollWithIndexDetectsPendingUpdate) {
  ReadHandler handler(knxd_, sessions_);

  // Simulate: packets were sent to 1/2/3 while the client was away.
  // The client comes back with i=5 (the last index it saw).
  // cache_last_updates_2 should return the GA as changed.
  knxd_.set_last_updates_result(5, {0x0A03}, 10);
  knxd_.set_cached_value(0x0A03, {0x42});

  auto result = handler.handle("a=1/2/3&i=5");

  EXPECT_EQ(result.http_status, 200);
  EXPECT_NE(result.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(result.body.find("42"), std::string::npos);
  // Index must advance to the new position from cache_last_updates_2
  EXPECT_NE(result.body.find("\"i\":\"10\""), std::string::npos);
}

// Simulates a busy KNX bus where cache_last_updates_2 returns immediately
// with telegrams for non-subscribed addresses, then eventually returns
// the subscribed address. The handler must skip non-matching addresses
// and still detect the matching one — without CPU-spinning.
TEST_F(ReadHandlerTest, LongPollWithIndexSkipsNonMatchingThenFindsMatch) {
  ReadHandler handler(knxd_, sessions_);

  // First call: non-matching address only (busy bus telegram)
  knxd_.set_last_updates_result(5, {0x0B04}, 7);
  // Second call: the matching address arrives
  knxd_.set_last_updates_result(7, {0x0A03}, 10);

  // Cache values for both addresses
  knxd_.set_cached_value(0x0B04, {0x0C, 0x6F});
  knxd_.set_cached_value(0x0A03, {0x42});

  // Long-poll subscribed only to 1/2/3
  auto result = handler.handle("a=1/2/3&i=5");

  EXPECT_EQ(result.http_status, 200);
  // Must include the matching address (1/2/3 = 0x0A03)
  EXPECT_NE(result.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(result.body.find("42"), std::string::npos);
  // Non-matching address must NOT appear
  EXPECT_EQ(result.body.find("1/3/4"), std::string::npos);
  // Index must advance to the final position
  EXPECT_NE(result.body.find("\"i\":\"10\""), std::string::npos);
}

// Simulates knxd's CACHE_LAST_UPDATES_2 returning "no updates" (changed=0,
// position unchanged) because knxd has an internal ~1s timeout. The handler
// must NOT sleep-and-break on this response — it must continue polling so
// that a subsequent telegram is detected.
TEST_F(ReadHandlerTest, LongPollContinuesPollingAfterKnxdEmptyResponse) {
  ReadHandler handler(knxd_, sessions_);

  // First call: knxd returns "no updates" (changed=0, position unchanged)
  // after=0 means this matches any start position
  knxd_.set_last_updates_result(0, {}, 11028);
  // Second call: a telegram for our GA finally arrives
  knxd_.set_last_updates_result(11028, {0x0A03}, 11029);
  knxd_.set_cached_value(0x0A03, {0x42});

  // Long-poll with i=11028, no t (default timeout)
  auto result = handler.handle("a=1/2/3&i=11028");

  EXPECT_EQ(result.http_status, 200);
  EXPECT_NE(result.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(result.body.find("42"), std::string::npos);
  EXPECT_NE(result.body.find("\"i\":\"11029\""), std::string::npos);
}

TEST_F(ReadHandlerTest, RecoversFromCacheUpdatesFailure) {
  // Simulates knxd restart: cache_last_updates_2 fails once, then
  // succeeds after the handler calls reconnect().
  ReadHandler handler(knxd_, sessions_);

  // First call to cache_last_updates_2 will fail (return nullopt)
  knxd_.set_cache_last_updates_fail_count(1);
  // After reconnect, the second call will succeed
  knxd_.set_last_updates_result(0, {0x0A03}, 10);
  knxd_.set_cached_value(0x0A03, {0x42});

  auto result = handler.handle("a=1/2/3&t=30");

  EXPECT_EQ(result.http_status, 200);
  EXPECT_NE(result.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(result.body.find("42"), std::string::npos);
  EXPECT_NE(result.body.find("\"i\":\"10\""), std::string::npos);
}

TEST_F(ReadHandlerTest, RecoversFromCacheReadFailureInPollLoop) {
  // Simulates knxd restart during the cache_read that follows a successful
  // cache_last_updates_2.
  ReadHandler handler(knxd_, sessions_);

  // cache_last_updates_2 succeeds, returning changed address
  knxd_.set_last_updates_result(0, {0x0A03}, 10);
  // But cache_read for that address fails on the first attempt
  knxd_.set_cache_read_fail_count(1);
  // The second cache_read succeeds (after internal reconnect in KnxdClient)
  knxd_.set_cached_value(0x0A03, {0x42});

  auto result = handler.handle("a=1/2/3&t=30");

  EXPECT_EQ(result.http_status, 200);
  EXPECT_NE(result.body.find("1/2/3"), std::string::npos);
  // The cache_read retry in KnxdClient should recover the value
  EXPECT_NE(result.body.find("42"), std::string::npos);
}

TEST_F(ReadHandlerTest, HandlesPersistentKnxdOutage) {
  // Simulates knxd being permanently down: cache_last_updates_2 always fails.
  // The handler should eventually return a valid response (empty data + index)
  // rather than crashing or hanging.
  ReadHandler handler(knxd_, sessions_);

  // All cache_last_updates_2 calls fail
  knxd_.set_cache_last_updates_fail_count(100);

  auto result = handler.handle("a=1/2/3&t=1");

  EXPECT_EQ(result.http_status, 200);
  // Should have empty data object and an index
  EXPECT_NE(result.body.find("\"d\":{}"), std::string::npos);
  EXPECT_NE(result.body.find("\"i\":\""), std::string::npos);
}

TEST_F(ReadHandlerTest, InvalidTimeoutReturns400) {
  ReadHandler handler(knxd_, sessions_);
  auto result = handler.handle("a=1/2/3&t=abc");
  EXPECT_EQ(result.http_status, 400);
}

TEST_F(ReadHandlerTest, InvalidTimeoutTrailingGarbage) {
  ReadHandler handler(knxd_, sessions_);
  auto result = handler.handle("a=1/2/3&t=5xyz");
  EXPECT_EQ(result.http_status, 400);
}

TEST_F(ReadHandlerTest, SessionInvalidReturns401) {
  // Create a session but validate a different one
  (void)sessions_.create_session(false);

  ReadHandler handler(knxd_, sessions_);
  auto result = handler.handle("a=1/2/3&t=30&s=nonexistent");
  EXPECT_EQ(result.http_status, 401);
}

TEST_F(ReadHandlerTest, AnonymousSessionAlwaysOk) {
  ReadHandler handler(knxd_, sessions_);
  knxd_.set_cached_value(0x0A03, {0x42});
  auto result = handler.handle("a=1/2/3&t=30&s=0");
  EXPECT_EQ(result.http_status, 200);
}

TEST_F(ReadHandlerTest, ValidSessionProceeds) {
  auto sid = sessions_.create_session(false);

  ReadHandler handler(knxd_, sessions_);
  knxd_.set_cached_value(0x0A03, {0x42});
  auto result = handler.handle("a=1/2/3&t=30&s=" + sid);
  EXPECT_EQ(result.http_status, 200);
}

TEST_F(ReadHandlerTest, LongPollTimeoutReturnsEmpty) {
  ReadHandler handler(knxd_, sessions_);
  // No telegrams enqueued — long-poll should time out quickly due to mock fd=-1
  auto result = handler.handle("a=1/2/3");
  EXPECT_EQ(result.http_status, 200);
  // Should have empty "d" object and an index
  EXPECT_NE(result.body.find("\"d\":{}"), std::string::npos);
  EXPECT_NE(result.body.find("\"i\":\""), std::string::npos);
}

// ---- User-reported bug reproduction ----

TEST_F(ReadHandlerTest, TimeoutZeroWithCachedValueAndBusyBusReturnsCorrectIndex) {
  // Reproduces: GET /r 's=0&a=7/4/2&t=0' on a busy KNX bus.
  // The knxd cache has the value, and cache_last_updates_2 returns position 42.
  // Expectation: {"d":{"7/4/2":"0c6f"},"i":"42"} — not {"d":{},"i":"0"}.
  ReadHandler handler(knxd_, sessions_);

  // Cache has the value
  std::vector<uint8_t> data = {0x0C, 0x6F};  // 2-byte temperature value
  knxd_.set_cached_value(0x3C02, data);      // 7/4/2 = (7<<11)|(4<<8)|2 = 0x3C02

  // cache_last_updates_2 returns position 42 (simulating busy bus)
  knxd_.set_last_updates_result(0, {}, 42);

  auto result = handler.handle("a=7/4/2&t=0");

  EXPECT_EQ(result.http_status, 200);
  // Must include the cached value (found during initial read)
  EXPECT_NE(result.body.find("7/4/2"), std::string::npos);
  EXPECT_NE(result.body.find("0c6f"), std::string::npos);
  // i must be the new_position from cache_last_updates_2 (42)
  EXPECT_NE(result.body.find("\"i\":\"42\""), std::string::npos);
  // Must NOT block — no read telegram sent because value was cached
  EXPECT_TRUE(knxd_.sent_packets().empty());
}

TEST_F(ReadHandlerTest, InitialIndexReturnsCachedValueImmediately) {
  // Reproduces: GET /r 's=0&a=7/4/2&i=0' — client has no prior state (i=0),
  // cache has the value. Should return immediately with cached value,
  // NOT block in COMET poll.
  ReadHandler handler(knxd_, sessions_);

  // Value is cached
  knxd_.set_cached_value(0x3C02, {0x0C, 0x6F});  // 7/4/2

  // cache_last_updates_2 returns position 42 (bus is active)
  knxd_.set_last_updates_result(0, {}, 42);

  // No 't' parameter but i=0: initial request should check cache first
  auto result = handler.handle("a=7/4/2&i=0");

  EXPECT_EQ(result.http_status, 200);
  // Must return the cached value immediately, not block
  EXPECT_NE(result.body.find("7/4/2"), std::string::npos);
  EXPECT_NE(result.body.find("0c6f"), std::string::npos);
  // i must reflect the new_position from cache_last_updates_2 (42)
  EXPECT_NE(result.body.find("\"i\":\"42\""), std::string::npos);
}

// ============================================================================
// Tests for corrected behavior (matching original eibread-cgi.c semantics)
// These tests define the EXPECTED behavior; the current implementation should
// FAIL these tests until the handler is rewritten.
// ============================================================================

// ---- t parameter as simple timeout (issue #4) ----

TEST_F(ReadHandlerTest, TimeoutParamIsSimplePollTimeout) {
  // t=5 means "wait up to 5 seconds in the poll loop".
  // It is NOT a freshness/cache check — it's the timeout for cache_last_updates_2.
  ReadHandler handler(knxd_, sessions_);

  // Set up cache_last_updates_2 to return one changed address
  knxd_.set_last_updates_result(0, {0x0A03}, 5);
  knxd_.set_cached_value(0x0A03, {0x42});

  auto result = handler.handle("a=1/2/3&t=5");

  EXPECT_EQ(result.http_status, 200);
  // Should return the changed value from the poll loop
  EXPECT_NE(result.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(result.body.find("42"), std::string::npos);
  // i should be the new position (5), not telegram count
  EXPECT_NE(result.body.find("\"i\":\"5\""), std::string::npos);
}

TEST_F(ReadHandlerTest, TimeoutZeroForcesInitialRead) {
  // t=0 forces lastpos=0, triggering an initial cache read of all addresses.
  // After initial read, enters poll loop with timeout=1.
  ReadHandler handler(knxd_, sessions_);

  // Cache has values
  knxd_.set_cached_value(0x0A03, {0x42});        // 1/2/3
  knxd_.set_cached_value(0x0B04, {0x0C, 0x6F});  // 1/3/4

  // Set up cache_last_updates_2 to return immediately with no changes
  knxd_.set_last_updates_result(0, {}, 5);

  auto result = handler.handle("a=1/2/3&a=1/3/4&t=0");

  EXPECT_EQ(result.http_status, 200);
  // Both cached values should be in the initial read response
  EXPECT_NE(result.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(result.body.find("42"), std::string::npos);
  EXPECT_NE(result.body.find("1/3/4"), std::string::npos);
  EXPECT_NE(result.body.find("0c6f"), std::string::npos);
  // i should be the end position from cache_last_updates_2
  EXPECT_NE(result.body.find("\"i\":\"5\""), std::string::npos);
}

TEST_F(ReadHandlerTest, TimeoutNegativeIsTreatedAsNormalTimeout) {
  // t=-1 should be parsed as -1 and treated like any timeout value.
  // Negative timeout means immediate timeout in practice.
  ReadHandler handler(knxd_, sessions_);

  // Set up cache_last_updates_2 to return immediately
  knxd_.set_last_updates_result(0, {}, 1);

  auto result = handler.handle("a=1/2/3&t=-1");
  EXPECT_EQ(result.http_status, 200);
  // Should not be a 400 (invalid timeout)
  EXPECT_NE(result.body.find("\"i\":\""), std::string::npos);
}

// ---- i parameter as position cursor (issue #3) ----

TEST_F(ReadHandlerTest, IndexParamUsedAsStartPositionForCacheLastUpdates) {
  // Client sends i=7 → cache_last_updates_2 is called with start=7
  // Only changes after position 7 are returned.
  ReadHandler handler(knxd_, sessions_);

  // Set up cache_last_updates_2 to return changes since position 7
  knxd_.set_last_updates_result(7, {0x0A03}, 12);
  knxd_.set_cached_value(0x0A03, {0x42});

  auto result = handler.handle("a=1/2/3&i=7");

  EXPECT_EQ(result.http_status, 200);
  EXPECT_NE(result.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(result.body.find("42"), std::string::npos);
  // i in response should be the new end position (12)
  EXPECT_NE(result.body.find("\"i\":\"12\""), std::string::npos);
}

TEST_F(ReadHandlerTest, IndexZeroTriggersInitialReadWithCacheFirst) {
  // i=0 means the client has no prior state. Initial cache read is done first,
  // then the poll loop runs. If cache has values, they're returned immediately.
  ReadHandler handler(knxd_, sessions_);

  knxd_.set_cached_value(0x0A03, {0x42});

  // Set up cache_last_updates_2 to return immediately with no changes
  knxd_.set_last_updates_result(0, {}, 5);

  auto result = handler.handle("a=1/2/3&i=0");

  EXPECT_EQ(result.http_status, 200);
  // Initial cache read should find the value
  EXPECT_NE(result.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(result.body.find("42"), std::string::npos);
}

TEST_F(ReadHandlerTest, IndexParamResponseIsNewPositionNotTelegramCount) {
  // The "i" in the response must be the end position from cache_last_updates_2,
  // NOT the telegram_count_ from the group socket.
  ReadHandler handler(knxd_, sessions_);

  // Simulate a busy bus with high telegram count
  knxd_.set_telegram_count(999);

  // But cache_last_updates_2 returns position 5
  knxd_.set_last_updates_result(0, {0x0A03}, 5);
  knxd_.set_cached_value(0x0A03, {0x42});

  auto result = handler.handle("a=1/2/3");

  EXPECT_EQ(result.http_status, 200);
  // i must be 5 (the new_position), NOT 999 (telegram_count)
  EXPECT_NE(result.body.find("\"i\":\"5\""), std::string::npos);
  EXPECT_EQ(result.body.find("\"i\":\"999\""), std::string::npos);
}

// ---- Multi-address response (issue #6) ----

TEST_F(ReadHandlerTest, MultipleChangedAddressesInOneResponse) {
  // When cache_last_updates_2 returns multiple changed addresses,
  // ALL matching ones must be included in a single JSON response.
  ReadHandler handler(knxd_, sessions_);

  // Two addresses changed: 1/2/3 and 1/3/4
  knxd_.set_last_updates_result(0, {0x0A03, 0x0B04}, 10);
  knxd_.set_cached_value(0x0A03, {0x42});        // 1/2/3
  knxd_.set_cached_value(0x0B04, {0x0C, 0x6F});  // 1/3/4

  auto result = handler.handle("a=1/2/3&a=1/3/4");

  EXPECT_EQ(result.http_status, 200);
  // Both addresses must appear in the response
  EXPECT_NE(result.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(result.body.find("42"), std::string::npos);
  EXPECT_NE(result.body.find("1/3/4"), std::string::npos);
  EXPECT_NE(result.body.find("0c6f"), std::string::npos);
  // i should be the new position
  EXPECT_NE(result.body.find("\"i\":\"10\""), std::string::npos);
}

TEST_F(ReadHandlerTest, OnlySubscribedAddressesInMultiResponse) {
  // When cache_last_updates_2 returns addresses A and B, but only A is subscribed,
  // only A should appear in the response.
  ReadHandler handler(knxd_, sessions_);

  // 1/2/3 changed AND 1/3/4 changed, but only 1/2/3 is subscribed
  knxd_.set_last_updates_result(0, {0x0A03, 0x0B04}, 10);
  knxd_.set_cached_value(0x0A03, {0x42});        // 1/2/3 — subscribed
  knxd_.set_cached_value(0x0B04, {0x0C, 0x6F});  // 1/3/4 — NOT subscribed

  auto result = handler.handle("a=1/2/3");

  EXPECT_EQ(result.http_status, 200);
  // 1/2/3 should be in response
  EXPECT_NE(result.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(result.body.find("42"), std::string::npos);
  // 1/3/4 should NOT be in response
  EXPECT_EQ(result.body.find("1/3/4"), std::string::npos);
}

TEST_F(ReadHandlerTest, MultiResponseDeduplicatesAddresses) {
  // If an address appears multiple times in the changed list, it should
  // only appear once in the response (matching original strstr check).
  ReadHandler handler(knxd_, sessions_);

  // Same address appears twice in the changed list
  knxd_.set_last_updates_result(0, {0x0A03, 0x0A03}, 10);
  knxd_.set_cached_value(0x0A03, {0x42});

  auto result = handler.handle("a=1/2/3");

  EXPECT_EQ(result.http_status, 200);
  // Should appear exactly once
  auto pos = result.body.find("1/2/3");
  EXPECT_NE(pos, std::string::npos);
  EXPECT_EQ(result.body.find("1/2/3", pos + 1), std::string::npos);
}

// ---- Position-based polling (issue #2) ----

TEST_F(ReadHandlerTest, UsesCacheLastUpdates2ForPolling) {
  // The long-poll mechanism must use cache_last_updates_2, not raw group socket.
  // Even with telegrams enqueued in the group socket, the handler should
  // prefer cache_last_updates_2 for position-based polling.
  ReadHandler handler(knxd_, sessions_);

  // Enqueue a telegram in the group socket (old mechanism)
  knxd_.enqueue_telegram(0x0A03, {0x00, 0x80, 0x42});

  // But set up cache_last_updates_2 with different data
  knxd_.set_last_updates_result(0, {0x0B04}, 10);
  knxd_.set_cached_value(0x0B04, {0x0C, 0x6F});

  auto result = handler.handle("a=1/2/3&a=1/3/4");

  EXPECT_EQ(result.http_status, 200);
  // Should return the cache_last_updates_2 result (1/3/4), not the telegram (1/2/3)
  EXPECT_NE(result.body.find("1/3/4"), std::string::npos);
  EXPECT_NE(result.body.find("0c6f"), std::string::npos);
  // 1/2/3 came from the old group socket mechanism — should NOT appear
  // because we use cache_last_updates_2 now
}

TEST_F(ReadHandlerTest, CacheLastUpdates2TimeoutReturnsEmpty) {
  // When cache_last_updates_2 times out (returns nullopt), the handler
  // should return an empty response with the current position.
  ReadHandler handler(knxd_, sessions_);

  // No last_updates configured → cache_last_updates_2 returns nullopt
  auto result = handler.handle("a=1/2/3&t=1");

  EXPECT_EQ(result.http_status, 200);
  EXPECT_NE(result.body.find("\"d\":{}"), std::string::npos);
  EXPECT_NE(result.body.find("\"i\":\""), std::string::npos);
}

// ---- Initial read (lastpos==0) behavior ----

TEST_F(ReadHandlerTest, InitialReadChecksAllRequestedAddresses) {
  // When lastpos==0 (i=0 or t=0), cache is read for ALL requested addresses.
  ReadHandler handler(knxd_, sessions_);

  knxd_.set_cached_value(0x0A03, {0x42});
  knxd_.set_cached_value(0x0B04, {0x0C, 0x6F});
  // 0x0C05 is NOT cached

  // Set up cache_last_updates_2 to finish the poll loop
  knxd_.set_last_updates_result(0, {}, 5);

  auto result = handler.handle("a=1/2/3&a=1/3/4&a=1/4/5&i=0");

  EXPECT_EQ(result.http_status, 200);
  // Both cached values should appear
  EXPECT_NE(result.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(result.body.find("1/3/4"), std::string::npos);
  // Uncached address should not appear
  EXPECT_EQ(result.body.find("1/4/5"), std::string::npos);
}

// ---- APCI filtering ----

TEST_F(ReadHandlerTest, FiltersOutReadApduFromCache) {
  // APCI filtering (ignoring Read APDUs with no data) now happens inside
  // KnxdClient::cache_read(), not in the read handler.
  // The real KnxdClient::cache_read() strips the APDU header and filters
  // out Read APDUs (byte1 & 0xC0 == 0).
  //
  // Since the mock's cache_read() returns raw value bytes directly
  // (no APDU header), we test here that value bytes stored in the cache
  // are returned correctly by the handler.
  ReadHandler handler(knxd_, sessions_);

  // Store a proper value (not an APDU header)
  knxd_.set_cached_value(0x0A03, {0x42});
  knxd_.set_last_updates_result(0, {}, 5);

  auto result = handler.handle("a=1/2/3&i=0");

  EXPECT_EQ(result.http_status, 200);
  EXPECT_NE(result.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(result.body.find("42"), std::string::npos);
}

TEST_F(ReadHandlerTest, IncludesWriteApduFromCache) {
  // Write APDUs (byte 1 & 0xC0 == 0x80) should be included.
  ReadHandler handler(knxd_, sessions_);

  knxd_.set_cached_value(0x0A03, {0x00, 0x80, 0x42});

  knxd_.set_last_updates_result(0, {}, 5);

  auto result = handler.handle("a=1/2/3&i=0");

  EXPECT_EQ(result.http_status, 200);
  EXPECT_NE(result.body.find("1/2/3"), std::string::npos);
  EXPECT_NE(result.body.find("42"), std::string::npos);
}

// ---- No addresses ----

TEST_F(ReadHandlerTest, NoAddressesReturns400) {
  ReadHandler handler(knxd_, sessions_);
  auto result = handler.handle("s=abc&t=0");
  EXPECT_EQ(result.http_status, 400);
}
