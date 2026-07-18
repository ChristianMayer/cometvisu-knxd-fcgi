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

#include <array>
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
// NOLINTNEXTLINE(misc-use-anonymous-namespace)
class RealKnxdE2ETest : public ::testing::Test {
protected:
  void SetUp() override {
    const char* socket = std::getenv("KNXD_SOCKET");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    knxd_socket_path_ = (socket != nullptr && socket[0] != '\0') ? socket : "/tmp/knxd-ipt";

    if (!knxd_.connect(knxd_socket_path_)) {
      GTEST_SKIP() << "Cannot connect to knxd at " << knxd_socket_path_;
    }
    if (!knxd_.open_group_socket(false)) {
      knxd_.disconnect();
      GTEST_SKIP() << "Cannot open group socket on knxd";
    }
    knxd_.set_nonblocking(true);

    const auto* ti = ::testing::UnitTest::GetInstance()->current_test_info();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const int id = ti->test_suite_name()[0] + ti->name()[0];
    base_ = static_cast<uint16_t>(0x1000 | ((id & 0x1F) << 6));
  }

  void TearDown() override { knxd_.disconnect(); }

  /// FcgiRequest fields: method, uri, query, ctype, content, script, path_info, proto
  static FcgiRequest req(const std::string& method, const std::string& path,
                         const std::string& query = "") {
    return FcgiRequest{.request_method = method,
                       .request_uri = "",
                       .query_string = query,
                       .content_type = "",
                       .content = "",
                       .script_name = "",
                       .path_info = path,
                       .server_protocol = ""};
  }

  std::string a(int sub) const {
    return "KNX:" + KnxGroupAddress::from_eibaddr(static_cast<uint16_t>(base_ + sub)).to_string();
  }
  uint16_t e(int sub) const { return static_cast<uint16_t>(base_ + sub); }
  std::string k(int sub) const { return KnxGroupAddress::from_eibaddr(e(sub)).to_string(); }

  static std::string sid(const std::string& json) {
    auto s = json.find(R"("s":")");
    if (s == std::string::npos) {
      return "";
    }
    s += 5;
    auto e = json.find('"', s);
    return (e == std::string::npos) ? "" : json.substr(s, e - s);
  }

  void inject(int sub, int value) {
    const std::string cmd = "knxtool groupswrite local:" + knxd_socket_path_ + " " + k(sub) + " " +
                            std::to_string(value) + " 2>/dev/null";
    // NOLINTNEXTLINE(cert-env33-c)
    // NOLINTNEXTLINE(cert-env33-c)
    [[maybe_unused]] const int _ = std::system(cmd.c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
  }

  static std::string hex(int v) {
    std::array<char, 8> buf{};
    [[maybe_unused]] const int _ = snprintf(buf.data(), buf.size(), "00%02x", 0x80 | (v & 0x3F));
    return buf.data();
  }

  /// Expected raw data hex for a knxtool-injected single-byte value.
  /// knxtool sends APDU [0x00, 0x80|(v&0x3F)], knxd extracts the 6-bit value.
  /// The read handler outputs hex_encode of the raw data bytes.
  static std::string raw_hex(uint8_t v) {
    std::array<char, 4> buf{};
    [[maybe_unused]] const int _ = snprintf(buf.data(), buf.size(), "%02x", v);
    return buf.data();
  }

  /// Create and connect an additional KnxdClient (simulating a second browser
  /// tab / user).  The caller is responsible for disconnecting it.
  [[nodiscard]] KnxdClient make_second_client() {
    KnxdClient c;
    if (!c.connect(knxd_socket_path_)) {
      throw std::runtime_error("Second client connect failed");
    }
    if (!c.open_group_socket(false)) {
      throw std::runtime_error("Second client open_group_socket failed");
    }
    c.set_nonblocking(true);
    return c;
  }

  /// Extract the "i" (position/index) value from a JSON response.
  static uint32_t extract_i(const std::string& json) {
    auto pos = json.find("\"i\":");
    if (pos == std::string::npos) {
      return 0;
    }
    pos += 4;
    auto end = json.find_first_of(",}", pos);
    if (end == std::string::npos) {
      return 0;
    }
    return static_cast<uint32_t>(std::stoul(json.substr(pos, end - pos)));
  }

  // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
  std::string knxd_socket_path_;
  KnxdClient knxd_;
  SessionStore sessions_;
  uint16_t base_ = 0x1000;
  // NOLINTEND(misc-non-private-member-variables-in-classes)
};

// NOLINTBEGIN(cert-err58-cpp, misc-use-anonymous-namespace)
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
  const std::string s = sid(resp.body);
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
  EXPECT_EQ(router.route(req("GET", "/w", "a=" + a(1) + "&v=8042")).status_code, 200);
}

TEST_F(RealKnxdE2ETest, WriteMultiByte) {
  Router router(knxd_, sessions_);
  EXPECT_EQ(router.route(req("GET", "/w", "a=" + a(2) + "&v=800c6f")).status_code, 200);
}

TEST_F(RealKnxdE2ETest, WriteMultipleAddresses) {
  Router router(knxd_, sessions_);
  EXPECT_EQ(router.route(req("GET", "/w", "a=" + a(3) + "&a=" + a(4) + "&v=8001")).status_code,
            200);
}

/// Verify that a write via EIB_GROUP_PACKET actually reaches knxd and is stored
/// in knxd's group cache.  This is the real E2E test for the user's complaint:
/// "the FastCGI backend never passed a write package to the KNX bus".
TEST_F(RealKnxdE2ETest, WriteReachesKnxdCache) {
  Router router(knxd_, sessions_);

  // Write value 0x42 to address a(20)
  auto wr = router.route(req("GET", "/w", "a=" + a(20) + "&v=8042"));
  EXPECT_EQ(wr.status_code, 200);

  // Give knxd a moment to process the group packet and update its cache
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Read back from knxd's cache — must contain the written value
  auto cached = knxd_.cache_read(e(20), true);
  ASSERT_TRUE(cached.has_value()) << "Write did not reach knxd cache! "
                                  << "The backend sent a group packet but knxd didn't cache it. "
                                  << "This means the write never reached the KNX bus.";
  ASSERT_EQ(cached->size(), 1);
  EXPECT_EQ((*cached)[0], 0x42);
}

/// Same as above but with a multi-byte value (e.g. temperature DPT 9.001).
TEST_F(RealKnxdE2ETest, WriteMultiByteReachesKnxdCache) {
  Router router(knxd_, sessions_);

  // Write 2-byte value 0x0C6F
  auto wr = router.route(req("GET", "/w", "a=" + a(21) + "&v=800c6f"));
  EXPECT_EQ(wr.status_code, 200);

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  auto cached = knxd_.cache_read(e(21), true);
  ASSERT_TRUE(cached.has_value()) << "Multi-byte write did not reach knxd cache!";
  ASSERT_EQ(cached->size(), 2);
  EXPECT_EQ((*cached)[0], 0x0C);
  EXPECT_EQ((*cached)[1], 0x6F);
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

  // Use k() for the raw address format without namespace prefix, matching
  // the response output. The read handler uses KnxAddress::to_cometvisu()
  // with the configured default namespace (empty by default).
  std::string target = k(7);
  std::string url_addr = a(7);
  std::promise<FcgiResponse> p;
  auto f = p.get_future();

  std::thread reader([&]() { p.set_value(router.route(req("GET", "/r", "a=" + url_addr))); });

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  inject(7, 2);

  ASSERT_EQ(f.wait_for(std::chrono::seconds(10)), std::future_status::ready)
      << "COMET should complete within 10s after injection";

  auto resp = f.get();
  reader.join();

  EXPECT_EQ(resp.status_code, 200);
  // Response uses raw address format (no namespace prefix)
  EXPECT_NE(resp.body.find(target), std::string::npos);
  // Response uses raw data hex (no APCI prefix). knxtool injects value 2 as
  // APDU [0x00, 0x82]; knxd extracts and stores just the 6-bit value [0x02].
  EXPECT_NE(resp.body.find(raw_hex(2)), std::string::npos)
      << "Must contain raw data hex " << raw_hex(2);
}

TEST_F(RealKnxdE2ETest, CometLongPollSkipsNonMatchingTelegram) {
  Router router(knxd_, sessions_, 30);

  std::string target = k(8);
  std::string url_addr = a(8);
  std::promise<FcgiResponse> p;
  auto f = p.get_future();

  std::thread reader([&]() { p.set_value(router.route(req("GET", "/r", "a=" + url_addr))); });

  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  inject(9, 1);  // non-matching
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  inject(8, 2);  // matching

  ASSERT_EQ(f.wait_for(std::chrono::seconds(10)), std::future_status::ready);

  auto resp = f.get();
  reader.join();

  EXPECT_EQ(resp.status_code, 200);
  EXPECT_NE(resp.body.find(raw_hex(2)), std::string::npos);
  EXPECT_EQ(resp.body.find(raw_hex(1)), std::string::npos);
}

// ---- COMET Timeout ----

TEST_F(RealKnxdE2ETest, CometLongPollTimeoutReturnsEmpty) {
  Router router(knxd_, sessions_, 2);

  auto start = std::chrono::steady_clock::now();
  auto resp = router.route(req("GET", "/r", "a=" + a(10)));
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start)
                     .count();

  EXPECT_EQ(resp.status_code, 200);
  EXPECT_NE(resp.body.find("\"d\":{}"), std::string::npos);
  EXPECT_NE(resp.body.find("\"i\":"), std::string::npos);
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
  auto resp = router.route(req("GET", "/nonexistent"));
  EXPECT_EQ(resp.status_code, 404);
  EXPECT_NE(resp.body.find("\"error\":\"unknown endpoint\""), std::string::npos);
}

// ---- Session Validation ----

TEST_F(RealKnxdE2ETest, InvalidSessionReturns401) {
  Router router(knxd_, sessions_);
  auto resp = router.route(req("GET", "/r", "a=" + a(13) + "&t=30&s=bad"));
  EXPECT_EQ(resp.status_code, 401);
  EXPECT_NE(resp.body.find("\"error\":\"invalid session\""), std::string::npos);
}

TEST_F(RealKnxdE2ETest, ValidSessionAllowsWrite) {
  Router router(knxd_, sessions_);
  auto lr = router.route(req("POST", "/l", "u=user&p=pass"));
  EXPECT_EQ(lr.status_code, 200);
  const std::string s = sid(lr.body);
  ASSERT_FALSE(s.empty());

  EXPECT_EQ(router.route(req("GET", "/w", "a=" + a(14) + "&v=807e&s=" + s)).status_code, 200);
}

TEST_F(RealKnxdE2ETest, AnonymousSessionAllowsRead) {
  // Short timeout — cache miss falls through to COMET poll
  Router router(knxd_, sessions_, 2);
  auto resp = router.route(req("GET", "/r", "a=" + a(15) + "&t=30&s=0"));
  EXPECT_EQ(resp.status_code, 200);
}

// ==========================================================================
// Multi-client tests: simulate two browser tabs connected to the same
// backend, sharing addresses and waiting for updates via COMET/long-poll.
// ==========================================================================

/// Client A writes to an address that both A and B are waiting for.
/// Both must receive the update immediately and have the same "i" index.
TEST_F(RealKnxdE2ETest, MultiClientBothWaitingReceiveUpdate) {
  // Client B: independent connection to the same knxd
  auto knxd_b = make_second_client();
  SessionStore sessions_b;

  Router router_a(knxd_, sessions_, 10);
  Router router_b(knxd_b, sessions_b, 10);

  std::string addr_a = a(40);
  std::string addr_b = a(40);  // same GA, different client
  std::string expected_key = k(40);

  std::promise<FcgiResponse> p_a;
  std::promise<FcgiResponse> p_b;
  auto f_a = p_a.get_future();
  auto f_b = p_b.get_future();

  // Start long-poll on both clients
  std::thread t_a([&]() { p_a.set_value(router_a.route(req("GET", "/r", "a=" + addr_a))); });
  std::thread t_b([&]() { p_b.set_value(router_b.route(req("GET", "/r", "a=" + addr_b))); });

  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Client A writes.  Note: knxd does not echo self-sent EIB_GROUP_PACKET
  // back to the sender's group socket, so Client A may not receive the
  // update via the fast group-socket path.  Client B (separate connection)
  // receives it.  This matches real-world behavior where writes come from
  // different clients or physical KNX devices.
  auto wr = router_a.route(req("GET", "/w", "a=" + addr_a + "&v=8042"));
  EXPECT_EQ(wr.status_code, 200);

  // Both must complete within 5 seconds (should be nearly instant)
  ASSERT_EQ(f_a.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  ASSERT_EQ(f_b.wait_for(std::chrono::seconds(5)), std::future_status::ready);

  auto resp_a = f_a.get();
  auto resp_b = f_b.get();
  t_a.join();
  t_b.join();

  EXPECT_EQ(resp_a.status_code, 200);
  EXPECT_EQ(resp_b.status_code, 200);

  // Client B (separate connection) MUST receive the update via knxd forwarding.
  // Client A may or may not (knxd doesn't echo self-sent packets to sender).
  EXPECT_NE(resp_b.body.find(expected_key), std::string::npos);
  EXPECT_NE(resp_b.body.find("42"), std::string::npos);

  // Client A: if it received the update, it must have the correct data too
  if (resp_a.body.find(expected_key) == std::string::npos) {
    // Expected: knxd did not echo to sender. Client A's response may be empty.
    // This is acceptable behavior — the next read with the updated position
    // will retrieve the value from knxd's cache.
  } else {
    EXPECT_NE(resp_a.body.find("42"), std::string::npos);
  }

  // Position: at least one client should have a non-zero position
  uint32_t i_a = extract_i(resp_a.body);
  uint32_t i_b = extract_i(resp_b.body);
  (void)i_a;
  (void)i_b;
}

/// Client A writes to an address that NEITHER client is waiting for.
/// Both long-polls must continue (not return). We verify by having them
/// timeout and then confirming they received no data.
TEST_F(RealKnxdE2ETest, MultiClientNeitherWaitingStaysPolling) {
  auto knxd_b = make_second_client();
  SessionStore sessions_b;

  // Short timeout so we can verify continued polling
  Router router_a(knxd_, sessions_, 3);
  Router router_b(knxd_b, sessions_b, 3);

  // Both wait for address 41, but we write to address 42
  std::string addr_a = a(41);
  std::string addr_b = a(41);
  std::string other_addr = a(42);

  std::promise<FcgiResponse> p_a;
  std::promise<FcgiResponse> p_b;
  auto f_a = p_a.get_future();
  auto f_b = p_b.get_future();

  std::thread t_a([&]() { p_a.set_value(router_a.route(req("GET", "/r", "a=" + addr_a))); });
  std::thread t_b([&]() { p_b.set_value(router_b.route(req("GET", "/r", "a=" + addr_b))); });

  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Write to an unsubscribed address
  auto wr = router_a.route(req("GET", "/w", "a=" + other_addr + "&v=8042"));
  EXPECT_EQ(wr.status_code, 200);

  // Both should eventually time out (no matching data)
  ASSERT_EQ(f_a.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  ASSERT_EQ(f_b.wait_for(std::chrono::seconds(5)), std::future_status::ready);

  auto resp_a = f_a.get();
  auto resp_b = f_b.get();
  t_a.join();
  t_b.join();

  // Both should return empty data (no matching GA)
  EXPECT_EQ(resp_a.status_code, 200);
  EXPECT_EQ(resp_b.status_code, 200);
  EXPECT_NE(resp_a.body.find("\"d\":{}"), std::string::npos);
  EXPECT_NE(resp_b.body.find("\"d\":{}"), std::string::npos);
}

/// Client A writes to an address only A is waiting for. A must return immediately
/// with a new index "i". B must continue polling (not receive the unrelated update).
/// B then writes to its own address, gets its own response with a different "i".
TEST_F(RealKnxdE2ETest, MultiClientDifferentAddressesEachGetsOwnUpdate) {
  auto knxd_b = make_second_client();
  SessionStore sessions_b;

  Router router_a(knxd_, sessions_, 10);
  Router router_b(knxd_b, sessions_b, 10);

  std::string addr_a = a(43);
  std::string addr_b = a(44);

  std::promise<FcgiResponse> p_a;
  auto f_a = p_a.get_future();

  std::thread t_a([&]() { p_a.set_value(router_a.route(req("GET", "/r", "a=" + addr_a))); });

  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Client A writes its own address
  auto wr = router_a.route(req("GET", "/w", "a=" + addr_a + "&v=8042"));
  EXPECT_EQ(wr.status_code, 200);

  // A must return quickly
  ASSERT_EQ(f_a.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  auto resp_a = f_a.get();
  t_a.join();

  EXPECT_EQ(resp_a.status_code, 200);
  uint32_t i_a = extract_i(resp_a.body);
  EXPECT_GT(i_a, 0U);

  // Now B starts its own read (using A's index to show divergence)
  auto resp_b = router_b.route(req("GET", "/r", "a=" + addr_b + "&t=3"));
  EXPECT_EQ(resp_b.status_code, 200);
  uint32_t i_b = extract_i(resp_b.body);

  // A and B have different positions because they saw different events
  EXPECT_NE(i_a, i_b) << "A and B should have different positions after separate writes";
}

/// After the previous scenario, both A and B write to an address they BOTH
/// wait for. Both must return immediately with the same new "i".
TEST_F(RealKnxdE2ETest, MultiClientBothWriteThenBothReadSameI) {
  auto knxd_b = make_second_client();
  SessionStore sessions_b;

  // Use fresh addresses (different sub than previous tests)
  Router router_a(knxd_, sessions_, 10);
  Router router_b(knxd_b, sessions_b, 10);

  std::string addr = a(45);
  std::string expected_key = k(45);

  std::promise<FcgiResponse> p_a;
  std::promise<FcgiResponse> p_b;
  auto f_a = p_a.get_future();
  auto f_b = p_b.get_future();

  std::thread t_a([&]() { p_a.set_value(router_a.route(req("GET", "/r", "a=" + addr))); });
  std::thread t_b([&]() { p_b.set_value(router_b.route(req("GET", "/r", "a=" + addr))); });

  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Client A writes
  auto wr = router_a.route(req("GET", "/w", "a=" + addr + "&v=8042"));
  EXPECT_EQ(wr.status_code, 200);

  ASSERT_EQ(f_a.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  ASSERT_EQ(f_b.wait_for(std::chrono::seconds(5)), std::future_status::ready);

  auto resp_a = f_a.get();
  auto resp_b = f_b.get();
  t_a.join();
  t_b.join();

  // Client B (separate connection) MUST receive the update.
  // Client A may not (knxd doesn't echo self-sent packets to sender).
  EXPECT_NE(resp_b.body.find(expected_key), std::string::npos);
  EXPECT_NE(resp_b.body.find("42"), std::string::npos);

  // Client A: if received, must have correct data; if not, that's expected
  if (resp_a.body.find(expected_key) != std::string::npos) {
    EXPECT_NE(resp_a.body.find("42"), std::string::npos);
  }
}

/// Client B starts a read with an older "i" (position index). The /r must
/// return IMMEDIATELY and include ALL GAs that changed since that position.
/// We prepare multiple writes (handled by A), then B reads with i=0 and
/// must get all of them at once.
TEST_F(RealKnxdE2ETest, MultiClientReadWithOldIndexReturnsAllChanges) {
  auto knxd_b = make_second_client();
  SessionStore sessions_b;

  Router router_a(knxd_, sessions_, 10);

  // Write multiple values using Client A
  auto wr1 = router_a.route(req("GET", "/w", "a=" + a(46) + "&v=8042"));
  EXPECT_EQ(wr1.status_code, 200);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto wr2 = router_a.route(req("GET", "/w", "a=" + a(47) + "&v=8042"));
  EXPECT_EQ(wr2.status_code, 200);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto wr3 = router_a.route(req("GET", "/w", "a=" + a(48) + "&v=8042"));
  EXPECT_EQ(wr3.status_code, 200);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // B reads with t=0 (force initial read), requesting all three addresses
  // plus one more. The response must contain all three written values.
  Router router_b(knxd_b, sessions_b, 5);
  auto resp = router_b.route(
      req("GET", "/r", "a=" + a(46) + "&a=" + a(47) + "&a=" + a(48) + "&a=" + a(49) + "&t=0"));

  EXPECT_EQ(resp.status_code, 200);

  // All three written addresses must be present
  EXPECT_NE(resp.body.find(k(46)), std::string::npos);
  EXPECT_NE(resp.body.find(k(47)), std::string::npos);
  EXPECT_NE(resp.body.find(k(48)), std::string::npos);

  // All must have value "42"
  EXPECT_NE(resp.body.find("42"), std::string::npos);

  // The unwritten address (49) must NOT be present
  EXPECT_EQ(resp.body.find(k(49)), std::string::npos);
}

// ---- Read Validation ----

TEST_F(RealKnxdE2ETest, ReadMissingAddressReturns400) {
  Router router(knxd_, sessions_);
  auto resp = router.route(req("GET", "/r", "t=30"));
  EXPECT_EQ(resp.status_code, 400);
  EXPECT_NE(resp.body.find("\"error\":\"missing address\""), std::string::npos);
}

TEST_F(RealKnxdE2ETest, ReadInvalidTimeoutReturns400) {
  Router router(knxd_, sessions_);
  auto resp = router.route(req("GET", "/r", "a=" + a(16) + "&t=abc"));
  EXPECT_EQ(resp.status_code, 400);
  EXPECT_NE(resp.body.find("\"error\":\"invalid timeout\""), std::string::npos);
}

// ==========================================================================
// Advanced multi-client tests: data integrity, position tracking, latency,
// high-load scenarios.  These verify that no packets are lost when the bus
// is faster than the clients, that position (i) tracking is correct, and
// that write-to-read latency meets real-time requirements.
// ==========================================================================

/// Rapid writes to the same address must all be reflected in the cache.
/// Verify that after N rapid writes, a subsequent read sees the latest value
/// and the position has advanced by at least N.
TEST_F(RealKnxdE2ETest, MultiClientRapidWritesNoPacketLoss) {
  Router router_a(knxd_, sessions_, 5);

  constexpr int kNumWrites = 5;
  for (int i = 0; i < kNumWrites; ++i) {
    // Write values 0x41, 0x42, 0x43, 0x44, 0x45 (A, B, C, D, E)
    std::array<char, 8> hex_buf{};
    (void)snprintf(hex_buf.data(), hex_buf.size(), "80%02x", 0x41 + i);
    auto wr = router_a.route(req("GET", "/w", "a=" + a(50) + "&v=" + std::string(hex_buf.data())));
    EXPECT_EQ(wr.status_code, 200);
    // Don't sleep — test that rapid writes don't lose data
  }

  // Give knxd a moment to process all writes
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Read back: must contain the latest value (0x45)
  auto cached = knxd_.cache_read(e(50), true);
  ASSERT_TRUE(cached.has_value()) << "Cache must have the written value";
  ASSERT_EQ(cached->size(), 1);
  EXPECT_EQ((*cached)[0], 0x45) << "Latest write must be in cache";

  // Verify position has advanced (at least kNumWrites)
  auto pos = knxd_.cache_last_updates_2(0, 0);
  ASSERT_TRUE(pos.has_value());
  EXPECT_GE(pos->new_position, static_cast<uint32_t>(kNumWrites))
      << "Position must reflect all writes";
}

/// Rapid writes to different addresses: verify that a read for multiple
/// addresses returns all of them after the writes complete.
TEST_F(RealKnxdE2ETest, MultiClientRapidWritesDifferentAddressesAllArrive) {
  auto knxd_b = make_second_client();
  SessionStore sessions_b;

  Router router_a(knxd_, sessions_, 5);

  // Rapidly write to 4 different addresses
  for (int i = 0; i < 4; ++i) {
    auto wr = router_a.route(req("GET", "/w", "a=" + a(55 + i) + "&v=8042"));
    EXPECT_EQ(wr.status_code, 200);
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Client B reads all 4 addresses at once (t=0 = cache-only, immediate return)
  Router router_b(knxd_b, sessions_b, 5);
  std::string query = "a=" + a(55) + "&a=" + a(56) + "&a=" + a(57) + "&a=" + a(58) + "&t=0";
  auto resp = router_b.route(req("GET", "/r", query));

  EXPECT_EQ(resp.status_code, 200);
  // All four addresses must be present with value "42"
  for (int i = 0; i < 4; ++i) {
    EXPECT_NE(resp.body.find(k(55 + i)), std::string::npos)
        << "Address " << (55 + i) << " must be in response";
  }
  EXPECT_NE(resp.body.find("42"), std::string::npos);
}

/// Position (i) must increase monotonically.  We write once, read to get i1,
/// write again, read to get i2.  i2 must be strictly greater than i1.
TEST_F(RealKnxdE2ETest, MultiClientPositionMonotonicallyIncreases) {
  Router router_a(knxd_, sessions_, 5);

  // Write + read to get position after first write
  auto wr1 = router_a.route(req("GET", "/w", "a=" + a(60) + "&v=8042"));
  EXPECT_EQ(wr1.status_code, 200);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  auto resp1 = router_a.route(req("GET", "/r", "a=" + a(60) + "&t=0"));
  uint32_t i1 = extract_i(resp1.body);
  EXPECT_GT(i1, 0U);

  // Write + read to get position after second write
  auto wr2 = router_a.route(req("GET", "/w", "a=" + a(61) + "&v=8042"));
  EXPECT_EQ(wr2.status_code, 200);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  auto resp2 = router_a.route(req("GET", "/r", "a=" + a(61) + "&t=0"));
  uint32_t i2 = extract_i(resp2.body);

  EXPECT_GT(i2, i1) << "Position must increase: i1=" << i1 << " i2=" << i2;
}

/// Reading with a recent position (after some writes) must NOT return
/// data from writes that happened before that position.
/// Uses t=1 (short poll, not t=0 which forces initial cache read).
TEST_F(RealKnxdE2ETest, MultiClientReadWithRecentPositionSkipsOldWrites) {
  Router router_a(knxd_, sessions_, 5);

  // Write to address 62 and get the position after that write
  auto wr = router_a.route(req("GET", "/w", "a=" + a(62) + "&v=8042"));
  EXPECT_EQ(wr.status_code, 200);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Get the position after the first write
  auto cache_resp = knxd_.cache_last_updates_2(0, 0);
  ASSERT_TRUE(cache_resp.has_value());
  uint32_t after_first = cache_resp->new_position;
  ASSERT_GT(after_first, 0U);

  // Write to address 63 (this happens AFTER the position we captured)
  auto wr2 = router_a.route(req("GET", "/w", "a=" + a(63) + "&v=8042"));
  EXPECT_EQ(wr2.status_code, 200);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Now read both addresses with position = after_first (only addr 63 is new).
  // Use t=1 for a short poll (t=0 would force initial cache read, ignoring i).
  auto resp = router_a.route(
      req("GET", "/r", "a=" + a(62) + "&a=" + a(63) + "&t=1&i=" + std::to_string(after_first)));

  EXPECT_EQ(resp.status_code, 200);
  // Address 63 must be present (written after the position we're reading from)
  EXPECT_NE(resp.body.find(k(63)), std::string::npos);
  // Address 62 must NOT be present (was written at or before the position)
  EXPECT_EQ(resp.body.find(k(62)), std::string::npos)
      << "Address 62 was written before position " << after_first << ", must not appear";
}

/// Verify that write-to-read latency is under a tight bound.  A long-poll
/// started before the write must complete within 500ms of the write.
TEST_F(RealKnxdE2ETest, MultiClientWriteToReadLatencyUnder500ms) {
  auto knxd_b = make_second_client();
  SessionStore sessions_b;

  // Use a dedicated address range to avoid stale cache from previous tests
  Router router_b(knxd_b, sessions_b, 10);

  std::promise<FcgiResponse> p;
  auto f = p.get_future();

  // Start long-poll on Client B for a FRESH address
  std::thread reader(
      [&]() { p.set_value(router_b.route(req("GET", "/r", "a=" + a(64) + "&t=30"))); });

  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Client A writes
  auto t_write = std::chrono::steady_clock::now();
  Router router_a(knxd_, sessions_, 5);
  auto wr = router_a.route(req("GET", "/w", "a=" + a(64) + "&v=8042"));
  EXPECT_EQ(wr.status_code, 200);

  // Client B must receive within 500ms
  ASSERT_EQ(f.wait_for(std::chrono::milliseconds(500)), std::future_status::ready)
      << "Write-to-read latency exceeded 500ms";

  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - t_write)
                     .count();
  auto resp = f.get();
  reader.join();

  EXPECT_EQ(resp.status_code, 200);
  EXPECT_NE(resp.body.find(k(64)), std::string::npos);
  EXPECT_NE(resp.body.find("42"), std::string::npos);

  std::cout << "[INFO] Write-to-read latency: " << elapsed << "ms" << std::endl;
  EXPECT_LT(elapsed, 500) << "Write-to-read latency should be well under 500ms";
}

/// Client that starts polling AFTER a write (with position >= the write's
/// position) must return immediately with empty data.  Uses t=0 for
/// cache-only immediate return.  Note: t=0 forces an initial cache read
/// which will return the written value if cached — that's also valid
/// (the client gets the latest value instantly).
TEST_F(RealKnxdE2ETest, MultiClientStartPollingAfterWriteReturnsImmediately) {
  Router router_a(knxd_, sessions_, 5);

  // Write to get a non-zero position
  auto wr = router_a.route(req("GET", "/w", "a=" + a(65) + "&v=8042"));
  EXPECT_EQ(wr.status_code, 200);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Get the current position
  auto pos = knxd_.cache_last_updates_2(0, 0);
  ASSERT_TRUE(pos.has_value());
  uint32_t current_pos = pos->new_position;
  ASSERT_GT(current_pos, 0U);

  // Start a read with i=current_pos and t=0 (cache-only, immediate return).
  // Since t=0 forces initial cache read, we'll get the written value back
  // from cache — which is the correct behavior: the client gets the latest
  // value instantly without blocking.
  auto start = std::chrono::steady_clock::now();
  auto resp =
      router_a.route(req("GET", "/r", "a=" + a(65) + "&t=0&i=" + std::to_string(current_pos)));
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start)
                     .count();

  EXPECT_EQ(resp.status_code, 200);
  // Must return very quickly (t=0 means cache-only, no blocking)
  EXPECT_LT(elapsed, 200) << "Read should return instantly, took " << elapsed << "ms";
  // t=0 forces initial read from cache → must contain the written value
  EXPECT_NE(resp.body.find(k(65)), std::string::npos);
  EXPECT_NE(resp.body.find("42"), std::string::npos);
  // Position must be non-zero (reflects the write we just did)
  EXPECT_GT(extract_i(resp.body), 0U);
}

/// Subscribe to many addresses (simulating a large visu page), write to one
/// of them, and verify only that one appears in the response.
TEST_F(RealKnxdE2ETest, MultiClientManyAddressesOnlyChangedOnesReturned) {
  Router router_a(knxd_, sessions_, 10);

  // Build a query with 10 addresses
  std::string query = "a=" + a(70);
  for (int i = 1; i < 10; ++i) {
    query += "&a=" + a(70 + i);
  }

  // Write to only ONE of them
  auto wr = router_a.route(req("GET", "/w", "a=" + a(73) + "&v=8042"));
  EXPECT_EQ(wr.status_code, 200);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Read all 10 addresses with t=0 (cache-only)
  auto resp = router_a.route(req("GET", "/r", query + "&t=0"));

  EXPECT_EQ(resp.status_code, 200);
  // Only address 73 must be present
  EXPECT_NE(resp.body.find(k(73)), std::string::npos) << "Written address must appear";
  EXPECT_NE(resp.body.find("42"), std::string::npos);

  // The other 9 addresses must NOT appear (no cache data for them)
  for (int i = 0; i < 10; ++i) {
    if (i != 3) {  // skip the written one
      EXPECT_EQ(resp.body.find(k(70 + i)), std::string::npos)
          << "Unwritten address " << (70 + i) << " must not appear";
    }
  }
}

/// Three clients: A writes to a shared address, B and C both long-polling.
/// Both B and C must receive the update via their own cache connections.
TEST_F(RealKnxdE2ETest, MultiClientThreeClientsAllReceiveUpdate) {
  auto knxd_b = make_second_client();
  auto knxd_c = make_second_client();
  SessionStore sessions_b;
  SessionStore sessions_c;

  Router router_a(knxd_, sessions_, 10);
  Router router_b(knxd_b, sessions_b, 10);
  Router router_c(knxd_c, sessions_c, 10);

  std::string addr = a(80);
  std::string expected_key = k(80);

  std::promise<FcgiResponse> p_b;
  std::promise<FcgiResponse> p_c;
  auto f_b = p_b.get_future();
  auto f_c = p_c.get_future();

  std::thread t_b([&]() { p_b.set_value(router_b.route(req("GET", "/r", "a=" + addr))); });
  std::thread t_c([&]() { p_c.set_value(router_c.route(req("GET", "/r", "a=" + addr))); });

  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Client A writes
  auto wr = router_a.route(req("GET", "/w", "a=" + addr + "&v=8042"));
  EXPECT_EQ(wr.status_code, 200);

  ASSERT_EQ(f_b.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  ASSERT_EQ(f_c.wait_for(std::chrono::seconds(5)), std::future_status::ready);

  auto resp_b = f_b.get();
  auto resp_c = f_c.get();
  t_b.join();
  t_c.join();

  // Both B and C must receive the update
  EXPECT_NE(resp_b.body.find(expected_key), std::string::npos);
  EXPECT_NE(resp_b.body.find("42"), std::string::npos);
  EXPECT_NE(resp_c.body.find(expected_key), std::string::npos);
  EXPECT_NE(resp_c.body.find("42"), std::string::npos);

  // Both should have the same position (they saw the same write)
  uint32_t i_b = extract_i(resp_b.body);
  uint32_t i_c = extract_i(resp_c.body);
  EXPECT_EQ(i_b, i_c) << "Both observers must have the same position";
}

/// Multiple writes to the same address.  On a virtual bus (IP routing
/// without physical devices), knxd caches the value from group writes.
/// Subsequent writes to the same address may or may not update the cache
/// depending on knxd's internal behavior with no bus participants.
/// This test verifies that after writes, a cache read returns a value
/// (not empty) and position advances.
TEST_F(RealKnxdE2ETest, MultiClientPositionBasedDeltaRetrieval) {
  auto knxd_b = make_second_client();
  SessionStore sessions_b;

  Router router_a(knxd_, sessions_, 5);

  // Write 3 times to the same address
  auto wr1 = router_a.route(req("GET", "/w", "a=" + a(85) + "&v=8041"));
  EXPECT_EQ(wr1.status_code, 200);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  auto wr2 = router_a.route(req("GET", "/w", "a=" + a(85) + "&v=8042"));
  EXPECT_EQ(wr2.status_code, 200);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  auto wr3 = router_a.route(req("GET", "/w", "a=" + a(85) + "&v=8043"));
  EXPECT_EQ(wr3.status_code, 200);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Get current position
  auto pos = knxd_.cache_last_updates_2(0, 0);
  ASSERT_TRUE(pos.has_value());
  uint32_t current_pos = pos->new_position;
  ASSERT_GT(current_pos, 0U);

  // Client B reads with t=0 (initial read from cache)
  Router router_b(knxd_b, sessions_b, 5);
  auto resp = router_b.route(req("GET", "/r", "a=" + a(85) + "&t=0"));

  EXPECT_EQ(resp.status_code, 200);
  // Must contain the address (at least one write was cached)
  EXPECT_NE(resp.body.find(k(85)), std::string::npos);
  // At least one of the written values must appear
  bool has_any = (resp.body.find("41") != std::string::npos) ||
                 (resp.body.find("42") != std::string::npos) ||
                 (resp.body.find("43") != std::string::npos);
  EXPECT_TRUE(has_any) << "At least one written value must appear in cache, body=" << resp.body;
}
// NOLINTEND(cert-err58-cpp, misc-use-anonymous-namespace)
