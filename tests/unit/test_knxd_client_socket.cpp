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
 * @file test_knxd_client.cpp
 * @brief Tests for KnxdClient::wait_for_activity() state reporting.
 *
 * The cache reader relies on being able to tell "no data yet" apart from
 * "the knxd connection is gone".  Reporting both as Timeout made the reader
 * loop spin at 100% CPU after knxd went away (see issue: "100% CPU when
 * knxd is missing").
 */

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <string>

#include "../../src/knxd/knxd_client.h"

namespace cvknxd {
namespace {

using WaitResult = KnxdClientInterface::WaitResult;

/// Minimal Unix stream socket listener that stands in for knxd.
class FakeKnxdListener {
public:
  FakeKnxdListener() {
    static int counter = 0;
    path_ = "/tmp/cvknxd-test-" + std::to_string(::getpid()) + "-" + std::to_string(counter++) +
            ".sock";
    fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) {
      return;
    }
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::bind(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
      ::close(fd_);
      fd_ = -1;
      return;
    }
    if (::listen(fd_, 4) != 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  ~FakeKnxdListener() {
    if (peer_ >= 0) {
      ::close(peer_);
    }
    if (fd_ >= 0) {
      ::close(fd_);
      ::unlink(path_.c_str());
    }
  }

  FakeKnxdListener(const FakeKnxdListener&) = delete;
  FakeKnxdListener& operator=(const FakeKnxdListener&) = delete;

  [[nodiscard]] bool is_valid() const { return fd_ >= 0; }
  [[nodiscard]] const std::string& path() const { return path_; }

  /// Accept the incoming client connection (blocking, must be called after connect()).
  int accept_one() {
    peer_ = ::accept(fd_, nullptr, nullptr);
    return peer_;
  }

  /// Close the server side of the connection — simulates knxd going away.
  void close_peer() {
    if (peer_ >= 0) {
      ::close(peer_);
      peer_ = -1;
    }
  }

private:
  std::string path_;
  int fd_ = -1;
  int peer_ = -1;
};

int64_t elapsed_ms(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                               start)
      .count();
}

TEST(KnxdClientTest, WaitForActivityReturnsErrorWhenNotConnected) {
  KnxdClient client;

  // No connection at all — this must NOT be reported as a plain timeout,
  // otherwise the cache reader busy-loops on it.
  EXPECT_EQ(client.wait_for_activity(10), WaitResult::Error);
}

TEST(KnxdClientTest, WaitForActivityReturnsTimeoutWhenNoDataArrives) {
  FakeKnxdListener listener;
  ASSERT_TRUE(listener.is_valid());

  KnxdClient client;
  ASSERT_TRUE(client.connect(listener.path()));
  ASSERT_GE(listener.accept_one(), 0);

  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(client.wait_for_activity(50), WaitResult::Timeout);
  // It must actually have blocked for roughly the requested time.
  EXPECT_GE(elapsed_ms(start), 30);
}

TEST(KnxdClientTest, WaitForActivityReturnsErrorWhenPeerClosesConnection) {
  FakeKnxdListener listener;
  ASSERT_TRUE(listener.is_valid());

  KnxdClient client;
  ASSERT_TRUE(client.connect(listener.path()));
  ASSERT_GE(listener.accept_one(), 0);

  // knxd terminates ("Link down, terminating") — the peer end disappears.
  listener.close_peer();

  // A closed peer produces POLLIN|POLLHUP on Linux; it must be reported as an
  // error, not as "data available" (which would loop forever).
  EXPECT_EQ(client.wait_for_activity(200), WaitResult::Error);
}

}  // namespace
}  // namespace cvknxd
