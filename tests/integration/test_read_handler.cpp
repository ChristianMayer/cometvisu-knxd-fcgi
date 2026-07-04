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

  auto result = handler.handle("a=KNX:1/2/3&t=30");

  EXPECT_EQ(result.http_status, 200);
  EXPECT_NE(result.body.find("KNX:1/2/3"), std::string::npos);
  EXPECT_NE(result.body.find("0c6f"), std::string::npos);
}

TEST_F(ReadHandlerTest, ReadFromKnxdCacheTimeoutZero) {
  ReadHandler handler(knxd_, sessions_);

  // Set up mock knxd cache
  std::vector<uint8_t> data = {0x42};
  knxd_.set_cached_value(0x0A03, data);

  auto result = handler.handle("a=KNX:1/2/3&t=0");

  EXPECT_EQ(result.http_status, 200);
  EXPECT_NE(result.body.find("KNX:1/2/3"), std::string::npos);
  EXPECT_NE(result.body.find("42"), std::string::npos);
}

TEST_F(ReadHandlerTest, NegativeTimeoutCacheMiss) {
  ReadHandler handler(knxd_, sessions_);

  // No knxd cache value — should return 404 (no data found)
  auto result = handler.handle("a=KNX:1/2/3&t=-1");
  EXPECT_EQ(result.http_status, 404);
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

  auto result = handler.handle("a=KNX:1/2/3&a=KNX:1/3/4&t=30");

  EXPECT_EQ(result.http_status, 200);
  EXPECT_NE(result.body.find("KNX:1/2/3"), std::string::npos);
  EXPECT_NE(result.body.find("42"), std::string::npos);
  EXPECT_NE(result.body.find("KNX:1/3/4"), std::string::npos);
  EXPECT_NE(result.body.find("0c6f"), std::string::npos);
}

TEST_F(ReadHandlerTest, LongPollGetsTelegram) {
  ReadHandler handler(knxd_, sessions_);

  // Enqueue a telegram that will be received during polling
  knxd_.enqueue_telegram(0x0A03, {0x00, 0x80, 0x42});

  // Long-poll (no timeout parameter)
  auto result = handler.handle("a=KNX:1/2/3");

  EXPECT_EQ(result.http_status, 200);
  EXPECT_NE(result.body.find("KNX:1/2/3"), std::string::npos);
  EXPECT_NE(result.body.find("42"), std::string::npos);
}

TEST_F(ReadHandlerTest, IndexIncluded) {
  ReadHandler handler(knxd_, sessions_);

  knxd_.set_cached_value(0x0A03, {0x42});
  auto result = handler.handle("a=KNX:1/2/3&t=30");

  EXPECT_NE(result.body.find("\"i\":\"1\""), std::string::npos);
}

TEST_F(ReadHandlerTest, InvalidAddressIgnored) {
  ReadHandler handler(knxd_, sessions_);

  knxd_.set_cached_value(0x0A03, {0x42});
  auto result = handler.handle("a=KNX:1/2/3&a=invalid&t=30");

  // Should still return the valid address
  EXPECT_EQ(result.http_status, 200);
  EXPECT_NE(result.body.find("KNX:1/2/3"), std::string::npos);
}

TEST_F(ReadHandlerTest, InvalidTimeoutReturns400) {
  ReadHandler handler(knxd_, sessions_);
  auto result = handler.handle("a=KNX:1/2/3&t=abc");
  EXPECT_EQ(result.http_status, 400);
}

TEST_F(ReadHandlerTest, InvalidTimeoutTrailingGarbage) {
  ReadHandler handler(knxd_, sessions_);
  auto result = handler.handle("a=KNX:1/2/3&t=5xyz");
  EXPECT_EQ(result.http_status, 400);
}

TEST_F(ReadHandlerTest, SessionInvalidReturns401) {
  // Create a session but validate a different one
  (void)sessions_.create_session(false);

  ReadHandler handler(knxd_, sessions_);
  auto result = handler.handle("a=KNX:1/2/3&t=30&s=nonexistent");
  EXPECT_EQ(result.http_status, 401);
}

TEST_F(ReadHandlerTest, AnonymousSessionAlwaysOk) {
  ReadHandler handler(knxd_, sessions_);
  knxd_.set_cached_value(0x0A03, {0x42});
  auto result = handler.handle("a=KNX:1/2/3&t=30&s=0");
  EXPECT_EQ(result.http_status, 200);
}

TEST_F(ReadHandlerTest, ValidSessionProceeds) {
  auto sid = sessions_.create_session(false);

  ReadHandler handler(knxd_, sessions_);
  knxd_.set_cached_value(0x0A03, {0x42});
  auto result = handler.handle("a=KNX:1/2/3&t=30&s=" + sid);
  EXPECT_EQ(result.http_status, 200);
}

TEST_F(ReadHandlerTest, LongPollTimeoutReturnsEmpty) {
  ReadHandler handler(knxd_, sessions_);
  // No telegrams enqueued — long-poll should time out quickly due to mock fd=-1
  auto result = handler.handle("a=KNX:1/2/3");
  EXPECT_EQ(result.http_status, 200);
  // Should have empty "d" object and an index
  EXPECT_NE(result.body.find("\"d\":{}"), std::string::npos);
  EXPECT_NE(result.body.find("\"i\":\""), std::string::npos);
}
