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

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <thread>

#include "fcgi/fcgi_request.h"
#include "knxd/knxd_client.h"
#include "knxd/knxd_protocol.h"
#include "router/router.h"
#include "state/session_store.h"
#include "util/hex.h"

using namespace cvknxd;

/// Real E2E tests against a running knxd instance.
///
/// Requires knxd (KNXD_SOCKET, default /tmp/knxd-ipt) and knxtool.
/// Telegram injection uses `knxtool groupswrite local:<socket>`.
///
/// knxtool injects via EIB_OPEN_T_GROUP; knxd forwards to group socket
/// clients as GROUP_PACKET (0x0027), handled by poll_group_telegram().
///
/// knxtool encodes value v → APCI 0x80|(v&0x3F), APDU [0x00, APCI],
/// hex-encoded as "00XX". Values 1→"0081", 2→"0082", 3→"0083".
///
/// Note: knxd does NOT cache T_Group injections, so cache_read always
/// returns empty (404). COMET/long-poll receives injected telegrams live.
class RealKnxdE2ETest : public ::testing::Test {
protected:
  void SetUp() override {
    const char* socket = std::getenv("KNXD_SOCKET");
    knxd_socket_path_ = (socket && socket[0] != '\0') ? socket : "/tmp/knxd-ipt";

    if (!knxd_.connect(knxd_socket_path_)) {
      GTEST_SKIP() << "Cannot connect to knxd at " << knxd_socket_path_;
    }
    if (!knxd_.open_group_socket(false)) {
      knxd_.disconnect();
      GTEST_SKIP() << "Cannot open group socket on knxd";
    }
    knxd_.set_nonblocking(true);

    const auto* ti = ::testing::UnitTest::GetInstance()->current_test_info();
    int id = ti->test_case_name()[0] + ti->name()[0];
    base_ = static_cast<uint16_t>(0x1000 | ((id & 0x1F) << 6));
  }

  void TearDown() override { knxd_.disconnect(); }

  /// FcgiRequest fields: method, uri, query, ctype, content, script, path_info, proto
  static FcgiRequest req(const std::string& method, const std::string& path,
                         const std::string& query = "") {
    return FcgiRequest{method, "", query, "", "", "", path, ""};
  }

  std::string a(int sub) const {
    return "KNX:" + KnxGroupAddress::from_eibaddr(
        static_cast<uint16_t>(base_ + sub)).to_string();
  }
  uint16_t e(int sub) const { return static_cast<uint16_t>(base_ + sub); }
  std::string k(int sub) const {
    return KnxGroupAddress::from_eibaddr(e(sub)).to_string();
  }

  static std::string sid(const std::string& json) {
    auto s = json.find("\"s\":\"");
    if (s == std::string::npos) return "";
    s += 5;
    auto e = json.find('"', s);
    return (e == std::string::npos) ? "" : json.substr(s, e - s);
  }

  void inject(int sub, int value) {
    std::string cmd = "knxtool groupswrite local:" + knxd_socket_path_ + " " +
                      k(sub) + " " + std::to_string(value) + " 2>/dev/null";
    (void)std::system(cmd.c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
  }

  static std::string hex(int v) {
    char buf[8];
    snprintf(buf, sizeof(buf), "00%02x", 0x80 | (v & 0x3F));
    return buf;
  }

  std::string knxd_socket_path_;
  KnxdClient knxd_;
  SessionStore sessions_;
  uint16_t base_ = 0x1000;
};

// ---- Login ----

TEST_F(RealKnxdE2ETest, LoginAnonymous) {
  Router router(knxd_, sessions_);
  auto resp = router.route(req("POST", "/l"));
  EXPECT_EQ(resp.status_code, 200);
  EXPECT_NE(resp.body.find("\"v\":\"0.0.2\""), std::string::npos);
  EXPECT_NE(resp.body.find("\"s\":\"0\""), std::string::npos);
}

TEST_F(RealKnxdE2ETest, LoginAuthenticated) {
  Router router(knxd_, sessions_);
  auto resp = router.route(req("POST", "/l", "u=admin&p=secret&d=test"));
  EXPECT_EQ(resp.status_code, 200);
  EXPECT_EQ(resp.body.find("\"s\":\"0\""), std::string::npos);
}

TEST_F(RealKnxdE2ETest, LoginCreatesValidSession) {
  Router router(knxd_, sessions_);
  auto resp = router.route(req("POST", "/l", "u=user&p=pass"));
  EXPECT_EQ(resp.status_code, 200);
  std::string s = sid(resp.body);
  ASSERT_FALSE(s.empty());
  EXPECT_NE(s, "0");
  EXPECT_TRUE(sessions_.is_valid(s));
}

// ---- Initial Read ----

TEST_F(RealKnxdE2ETest, InitialReadTimedPollReturnsEmpty) {
  // Use short longpoll timeout (2s) since t=0 cache-miss falls through to COMET poll
  Router router(knxd_, sessions_, 2);
  auto resp = router.route(req("GET", "/r", "a=" + a(99) + "&t=0"));
  EXPECT_EQ(resp.status_code, 200);
  EXPECT_NE(resp.body.find("\"d\":{}"), std::string::npos);
}

// ---- Write ----

TEST_F(RealKnxdE2ETest, WriteSingleByte) {
  Router router(knxd_, sessions_);
  EXPECT_EQ(router.route(req("GET", "/w", "a=" + a(1) + "&v=42")).status_code, 200);
}

TEST_F(RealKnxdE2ETest, WriteMultiByte) {
  Router router(knxd_, sessions_);
  EXPECT_EQ(router.route(req("GET", "/w", "a=" + a(2) + "&v=0c6f")).status_code, 200);
}

TEST_F(RealKnxdE2ETest, WriteMultipleAddresses) {
  Router router(knxd_, sessions_);
  EXPECT_EQ(router.route(req("GET", "/w",
      "a=" + a(3) + "&a=" + a(4) + "&v=01")).status_code, 200);
}

// ---- Read with timeout — returns 200 (empty on virtual bus) ----

TEST_F(RealKnxdE2ETest, ReadWithTimeoutReturnsEmpty) {
  // Short longpoll timeout — cache miss falls through to COMET poll
  Router router(knxd_, sessions_, 2);
  auto resp = router.route(req("GET", "/r", "a=" + a(5) + "&t=30"));
  EXPECT_EQ(resp.status_code, 200);
  EXPECT_NE(resp.body.find("\"d\":{}"), std::string::npos);
}

TEST_F(RealKnxdE2ETest, ReadCacheOnlyReturnsEmpty) {
  Router router(knxd_, sessions_);
  // t=-1: cache-only — returns 200 with empty data (never 404)
  auto resp = router.route(req("GET", "/r", "a=" + a(6) + "&t=-1"));
  EXPECT_EQ(resp.status_code, 200);
  EXPECT_NE(resp.body.find("\"d\":{}"), std::string::npos);
}

// ---- COMET Long-Poll — blocks until knxtool injects a telegram ----

TEST_F(RealKnxdE2ETest, CometLongPollReceivesInjectedTelegram) {
  Router router(knxd_, sessions_, 30);

  std::string target = a(7);
  std::promise<FcgiResponse> p;
  auto f = p.get_future();

  std::thread reader([&]() {
    p.set_value(router.route(req("GET", "/r", "a=" + target)));
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  inject(7, 2);

  ASSERT_EQ(f.wait_for(std::chrono::seconds(10)), std::future_status::ready)
      << "COMET should complete within 10s after injection";

  auto resp = f.get();
  reader.join();

  EXPECT_EQ(resp.status_code, 200);
  EXPECT_NE(resp.body.find(target), std::string::npos);
  EXPECT_NE(resp.body.find(hex(2)), std::string::npos)
      << "Must contain APDU hex " << hex(2);
}

TEST_F(RealKnxdE2ETest, CometLongPollSkipsNonMatchingTelegram) {
  Router router(knxd_, sessions_, 30);

  std::string target = a(8);
  std::promise<FcgiResponse> p;
  auto f = p.get_future();

  std::thread reader([&]() {
    p.set_value(router.route(req("GET", "/r", "a=" + target)));
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  inject(9, 1);  // non-matching
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  inject(8, 2);  // matching

  ASSERT_EQ(f.wait_for(std::chrono::seconds(10)), std::future_status::ready);

  auto resp = f.get();
  reader.join();

  EXPECT_EQ(resp.status_code, 200);
  EXPECT_NE(resp.body.find(hex(2)), std::string::npos);
  EXPECT_EQ(resp.body.find(hex(1)), std::string::npos);
}

// ---- COMET Timeout ----

TEST_F(RealKnxdE2ETest, CometLongPollTimeoutReturnsEmpty) {
  Router router(knxd_, sessions_, 2);

  auto start = std::chrono::steady_clock::now();
  auto resp = router.route(req("GET", "/r", "a=" + a(10)));
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start).count();

  EXPECT_EQ(resp.status_code, 200);
  EXPECT_NE(resp.body.find("\"d\":{}"), std::string::npos);
  EXPECT_NE(resp.body.find("\"i\":\""), std::string::npos);
  EXPECT_GE(elapsed, 1800) << "Should block ~2s, only " << elapsed << "ms";
}

// ---- Write Validation (never 400) ----

TEST_F(RealKnxdE2ETest, WriteMissingAddressReturns200) {
  Router router(knxd_, sessions_);
  // Missing address: nothing to write → 200 (no-op)
  EXPECT_EQ(router.route(req("GET", "/w", "v=42")).status_code, 200);
}

TEST_F(RealKnxdE2ETest, WriteMissingValueReturns200) {
  Router router(knxd_, sessions_);
  // Missing value: nothing to write → 200 (no-op)
  EXPECT_EQ(router.route(req("GET", "/w", "a=" + a(11))).status_code, 200);
}

TEST_F(RealKnxdE2ETest, WriteInvalidHexReturns200) {
  Router router(knxd_, sessions_);
  // Invalid hex: cannot decode → 200 (no-op)
  EXPECT_EQ(router.route(req("GET", "/w", "a=" + a(12) + "&v=ZZ")).status_code, 200);
}

// ---- Router ----

TEST_F(RealKnxdE2ETest, RouterUnknownEndpointReturns404) {
  Router router(knxd_, sessions_);
  EXPECT_EQ(router.route(req("GET", "/nonexistent")).status_code, 404);
}

// ---- Session Validation ----

TEST_F(RealKnxdE2ETest, InvalidSessionReturns401) {
  Router router(knxd_, sessions_);
  EXPECT_EQ(router.route(req("GET", "/r",
      "a=" + a(13) + "&t=30&s=bad")).status_code, 401);
}

TEST_F(RealKnxdE2ETest, ValidSessionAllowsWrite) {
  Router router(knxd_, sessions_);
  auto lr = router.route(req("POST", "/l", "u=user&p=pass"));
  EXPECT_EQ(lr.status_code, 200);
  std::string s = sid(lr.body);
  ASSERT_FALSE(s.empty());

  EXPECT_EQ(router.route(req("GET", "/w",
      "a=" + a(14) + "&v=7e&s=" + s)).status_code, 200);
}

TEST_F(RealKnxdE2ETest, AnonymousSessionAllowsRead) {
  // Short timeout — cache miss falls through to COMET poll
  Router router(knxd_, sessions_, 2);
  auto resp = router.route(req("GET", "/r",
      "a=" + a(15) + "&t=30&s=0"));
  EXPECT_EQ(resp.status_code, 200);
}

// ---- Read Validation ----

TEST_F(RealKnxdE2ETest, ReadMissingAddressReturns400) {
  Router router(knxd_, sessions_);
  EXPECT_EQ(router.route(req("GET", "/r", "t=30")).status_code, 400);
}

TEST_F(RealKnxdE2ETest, ReadInvalidTimeoutReturns400) {
  Router router(knxd_, sessions_);
  EXPECT_EQ(router.route(req("GET", "/r",
      "a=" + a(16) + "&t=abc")).status_code, 400);
}
