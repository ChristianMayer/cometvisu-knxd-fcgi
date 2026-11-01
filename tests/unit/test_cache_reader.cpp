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
 * @file test_cache_reader.cpp
 * @brief Tests for the knxd -> SharedGroupCache reader loop.
 *
 * The loop must survive an absent or dying knxd: it waits between reconnect
 * attempts (never spinning), retries forever, and resumes pushing telegrams
 * once knxd comes back.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

#include "../../mocks/mock_knxd_socket.h"
#include "../../src/knxd/cache_reader.h"
#include "../../src/state/shared_group_cache.h"

namespace cvknxd {
namespace {

using Clock = std::chrono::steady_clock;

/// Options with tiny delays so tests run fast while still measuring real sleeps.
CacheReaderOptions FastOptions() {
  CacheReaderOptions options;
  options.socket_path = "/nonexistent/knx";
  options.wait_timeout_ms = 5;
  options.reconnect_backoff_ms = 20;
  options.max_reconnect_backoff_ms = 20;
  options.sleep_slice_ms = 2;
  return options;
}

int64_t ElapsedMs(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

TEST(CacheReaderTest, StopsImmediatelyWithoutTouchingKnxdWhenAlreadyStopped) {
  MockKnxdClient knxd;
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());

  run_cache_reader(knxd, cache, FastOptions(), [] { return true; });

  EXPECT_EQ(knxd.connect_calls(), 0);
  EXPECT_EQ(knxd.wait_calls(), 0);
}

TEST(CacheReaderTest, WaitsBetweenReconnectAttemptsWhenKnxdIsMissing) {
  MockKnxdClient knxd;
  knxd.set_connection_success(false);  // knxd is not there (no /run/knx)
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());

  const auto start = Clock::now();
  // Stop after the fourth failed connect attempt.
  run_cache_reader(knxd, cache, FastOptions(), [&knxd] { return knxd.connect_calls() >= 4; });

  EXPECT_GE(knxd.connect_calls(), 4);
  // Three backoff sleeps of 20 ms must have happened — a busy loop would
  // return in well under a millisecond and fail here.
  EXPECT_GE(ElapsedMs(start), 50);
}

TEST(CacheReaderTest, ReconnectsWhenTheConnectionIsLost) {
  MockKnxdClient knxd;
  knxd.set_connection_success(true);
  knxd.set_wait_error_count(3);  // first three waits report a dead connection
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());

  const auto start = Clock::now();
  // 1 initial connect + 3 reconnects.
  run_cache_reader(knxd, cache, FastOptions(), [&knxd] { return knxd.connect_calls() >= 4; });

  EXPECT_GE(knxd.connect_calls(), 4);
  // One backoff sleep per lost connection — again, no busy loop.
  EXPECT_GE(ElapsedMs(start), 50);
}

TEST(CacheReaderTest, DoesNotReconnectWhileTheConnectionIsHealthy) {
  MockKnxdClient knxd;
  knxd.set_connection_success(true);
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());

  run_cache_reader(knxd, cache, FastOptions(), [&knxd] { return knxd.wait_calls() >= 20; });

  EXPECT_EQ(knxd.connect_calls(), 1);
  EXPECT_GE(knxd.wait_calls(), 20);
}

TEST(CacheReaderTest, PushesReceivedTelegramsIntoSharedCache) {
  MockKnxdClient knxd;
  knxd.set_connection_success(true);
  // APDU for A_GroupValue_Write with value 0x42.
  knxd.enqueue_telegram(0x0A03, {0x00, 0x80, 0x42});
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());

  run_cache_reader(knxd, cache, FastOptions(), [&cache] { return cache.get(0x0A03).has_value(); });

  const auto value = cache.get(0x0A03);
  ASSERT_TRUE(value.has_value());
  ASSERT_EQ(value->size(), 1U);
  EXPECT_EQ((*value)[0], 0x42);
  EXPECT_EQ(cache.position(), 1U);
}

TEST(CacheReaderTest, IgnoresGroupValueReadTelegrams) {
  MockKnxdClient knxd;
  knxd.set_connection_success(true);
  // A_GroupValue_Read has no value — it must not enter the cache.
  knxd.enqueue_telegram(0x0A03, {0x00, 0x00});
  SharedGroupCache cache;
  ASSERT_TRUE(cache.create());

  run_cache_reader(knxd, cache, FastOptions(), [&knxd] { return knxd.wait_calls() >= 5; });

  EXPECT_FALSE(cache.get(0x0A03).has_value());
  EXPECT_EQ(cache.position(), 0U);
}

}  // namespace
}  // namespace cvknxd
