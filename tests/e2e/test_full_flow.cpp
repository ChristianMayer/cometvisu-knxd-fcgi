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

#include "fcgi/fcgi_request.h"
#include "handlers/login_handler.h"
#include "handlers/read_handler.h"
#include "handlers/write_handler.h"
#include "knxd/knxd_protocol.h"
#include "mock_knxd_socket.h"
#include "router/router.h"
#include "state/session_store.h"

using namespace cvknxd;

/// End-to-end tests simulating full CometVisu protocol flows.
class FullFlowTest : public ::testing::Test {
protected:
  void SetUp() override {
    (void)knxd_.connect("/run/knx");
    (void)knxd_.open_group_socket(false);
  }

  MockKnxdClient knxd_;
  SessionStore sessions_;
};

// ---- Login Flow ----

TEST_F(FullFlowTest, AnonymousLogin) {
  LoginHandler handler(sessions_);

  std::string response = handler.handle("");
  EXPECT_NE(response.find("\"v\":\"1.0\""), std::string::npos);
  EXPECT_NE(response.find("\"s\":\"0\""), std::string::npos);
}

TEST_F(FullFlowTest, AuthenticatedLogin) {
  LoginHandler handler(sessions_);

  std::string response = handler.handle("u=admin&p=secret&d=mydevice");
  EXPECT_NE(response.find("\"v\":\"1.0\""), std::string::npos);

  // Session ID should NOT be "0"
  EXPECT_EQ(response.find("\"s\":\"0\""), std::string::npos);
}

// ---- Write then Read Flow ----

TEST_F(FullFlowTest, WriteThenRead) {
  WriteHandler write_handler(knxd_, sessions_);
  ReadHandler read_handler(knxd_, sessions_);

  // Step 1: Write a value
  auto write_result = write_handler.handle("a=KNX:1/2/3&v=0c6f");
  EXPECT_EQ(write_result.http_status, 200);

  // Step 2: Pre-populate mock knxd cache (simulating knxd's auto-caching)
  knxd_.set_cached_value(0x0A03, {0x0C, 0x6F});

  // Step 3: Read from knxd's cache
  auto read_result = read_handler.handle("a=KNX:1/2/3&t=30");
  EXPECT_EQ(read_result.http_status, 200);
  EXPECT_NE(read_result.body.find("0c6f"), std::string::npos);
}

// ---- Router Dispatch ----

TEST_F(FullFlowTest, RouterLoginRoute) {
  Router router(knxd_, sessions_);

  FcgiRequest req;
  req.request_method = "GET";
  req.path_info = "/l";
  req.query_string = "";

  auto resp = router.route(req);
  EXPECT_EQ(resp.status_code, 200);
  EXPECT_NE(resp.body.find("\"v\":\"1.0\""), std::string::npos);
}

TEST_F(FullFlowTest, RouterReadRoute) {
  Router router(knxd_, sessions_);

  knxd_.set_cached_value(0x0A03, {0x42});

  FcgiRequest req;
  req.request_method = "GET";
  req.path_info = "/r";
  req.query_string = "a=KNX:1/2/3&t=30";

  auto resp = router.route(req);
  EXPECT_EQ(resp.status_code, 200);
  EXPECT_NE(resp.body.find("42"), std::string::npos);
}

TEST_F(FullFlowTest, RouterWriteRoute) {
  Router router(knxd_, sessions_);

  FcgiRequest req;
  req.request_method = "GET";
  req.path_info = "/w";
  req.query_string = "a=KNX:1/2/3&v=42";

  auto resp = router.route(req);
  EXPECT_EQ(resp.status_code, 200);

  auto sent = knxd_.sent_packets();
  ASSERT_EQ(sent.size(), 1);
}

TEST_F(FullFlowTest, RouterUnknownRoute) {
  Router router(knxd_, sessions_);

  FcgiRequest req;
  req.request_method = "GET";
  req.path_info = "/unknown";
  req.query_string = "";

  auto resp = router.route(req);
  EXPECT_EQ(resp.status_code, 404);
}

// ---- Response JSON Structure ----

TEST_F(FullFlowTest, LoginResponseStructure) {
  LoginHandler handler(sessions_);
  std::string response = handler.handle("u=admin&p=secret");

  // Should be valid JSON object with v and s keys
  EXPECT_EQ(response.front(), '{');
  EXPECT_EQ(response.back(), '}');
  EXPECT_NE(response.find("\"v\":\"1.0\""), std::string::npos);
  EXPECT_NE(response.find("\"s\":"), std::string::npos);
}

TEST_F(FullFlowTest, ReadResponseStructure) {
  ReadHandler handler(knxd_, sessions_);
  knxd_.set_cached_value(0x0A03, {0x42});

  auto result = handler.handle("a=KNX:1/2/3&t=30");

  // Should have "d" object and "i" index
  EXPECT_NE(result.body.find("\"d\":"), std::string::npos);
  EXPECT_NE(result.body.find("\"i\":"), std::string::npos);
}

// ---- Knxd Cache Integration ----

TEST_F(FullFlowTest, KnxdCacheIsUsedForReads) {
  // knxd's built-in cache stores values after writes or received telegrams.
  // Our code queries knxd's cache via cache_read() instead of a local cache.
  std::vector<uint8_t> data = {0x0C, 0x6F};
  knxd_.set_cached_value(0x0A03, data);

  ReadHandler handler(knxd_, sessions_);
  auto result = handler.handle("a=KNX:1/2/3&t=30");
  EXPECT_NE(result.body.find("0c6f"), std::string::npos);
}
