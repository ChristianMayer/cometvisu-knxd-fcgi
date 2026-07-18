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
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "knxd/knxd_client.h"

using namespace cvknxd;

namespace {

/// Generate a unique socket path for this test run.
std::string make_temp_socket_path() {
  const char* tmpdir = getenv("TMPDIR");
  if (tmpdir == nullptr || std::strlen(tmpdir) == 0) {
    tmpdir = "/tmp";
  }
  std::string tmpl = std::string(tmpdir) + "/knxd-concurrency-test-XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  int fd = mkstemp(buf.data());
  if (fd < 0) {
    return "";
  }
  close(fd);
  unlink(buf.data());
  return std::string(buf.data()) + ".sock";
}

/// Minimal fake knxd server that speaks enough of the eibd protocol
/// to support OPEN_GROUPCON, GROUP_PACKET, CACHE_READ, and CACHE_LAST_UPDATES_2.
/// Used to verify concurrency: a write on the main connection must not
/// be blocked by a long-poll (cache_last_updates_2) on the cache connection.
class FakeKnxdServer {
public:
  /// Record of a received group packet.
  struct ReceivedGroupPacket {
    uint16_t group_addr;
    std::vector<uint8_t> apdu;
  };
  FakeKnxdServer() {
    socket_path_ = make_temp_socket_path();
    if (socket_path_.empty()) {
      return;
    }

    listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
      return;
    }

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    size_t path_len = socket_path_.copy(addr.sun_path, sizeof(addr.sun_path) - 1);
    addr.sun_path[path_len] = '\0';

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
      return;
    }

    if (::listen(listen_fd_, 2) < 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
      return;
    }

    accept_thread_ = std::thread(&FakeKnxdServer::accept_loop, this);
  }

  ~FakeKnxdServer() {
    shutdown_.store(true);
    // Connect to ourselves to unblock accept()
    if (!socket_path_.empty()) {
      int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
      if (fd >= 0) {
        struct sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        size_t path_len = socket_path_.copy(addr.sun_path, sizeof(addr.sun_path) - 1);
        addr.sun_path[path_len] = '\0';
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        int conn_ret = ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
        (void)conn_ret;
        ::close(fd);
      }
    }
    if (accept_thread_.joinable()) {
      accept_thread_.join();
    }
    if (listen_fd_ >= 0) {
      ::close(listen_fd_);
    }
    if (!socket_path_.empty()) {
      ::unlink(socket_path_.c_str());
    }
  }

  // Non-copyable, non-movable (owns threads and file descriptors)
  FakeKnxdServer(const FakeKnxdServer&) = delete;
  FakeKnxdServer& operator=(const FakeKnxdServer&) = delete;
  FakeKnxdServer(FakeKnxdServer&&) = delete;
  FakeKnxdServer& operator=(FakeKnxdServer&&) = delete;

  [[nodiscard]] bool is_ready() const { return listen_fd_ >= 0; }
  [[nodiscard]] const std::string& path() const { return socket_path_; }

  /// Get all group packets received by the fake server.
  [[nodiscard]] std::vector<ReceivedGroupPacket> received_packets() const {
    std::lock_guard<std::mutex> lock(recv_mutex_);
    return received_packets_;
  }

private:
  /// Read exactly N bytes from fd. Returns false on EOF/error.
  static bool read_exact(int fd, uint8_t* buf, size_t len) {
    size_t off = 0;
    while (off < len) {
      ssize_t n = ::read(fd, &buf[off],
                         len - off);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      if (n <= 0) {
        return false;
      }
      off += static_cast<size_t>(n);
    }
    return true;
  }

  /// Write all bytes to fd. Returns false on error.
  static bool write_all(int fd, const uint8_t* data, size_t len) {
    size_t off = 0;
    while (off < len) {
      ssize_t n = ::write(fd, &data[off],
                          len - off);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      if (n <= 0) {
        return false;
      }
      off += static_cast<size_t>(n);
    }
    return true;
  }

  /// Handle a single client connection.
  /// This is intentionally minimal — it only supports the messages
  /// needed for the concurrency test.
  void handle_client(int fd) {
    // Read the first message to determine if this is a main or cache connection
    while (!shutdown_.load()) {
      // Read 2-byte length prefix
      std::array<uint8_t, 2> len_buf{};
      if (!read_exact(fd, len_buf.data(), 2)) {
        break;
      }
      uint16_t payload_len = static_cast<uint16_t>((len_buf[0] << 8) | len_buf[1]);

      // Read payload
      std::vector<uint8_t> payload(payload_len);
      if (payload_len > 0 && !read_exact(fd, payload.data(), payload_len)) {
        break;
      }

      if (payload_len < 2) {
        continue;
      }

      uint16_t msg_type = static_cast<uint16_t>((payload[0] << 8) | payload[1]);

      if (msg_type == EIB_OPEN_GROUPCON) {
        // Respond with success: same type, empty payload.
        // Wire format: [len:2][type:2].  len=2, payload is the 2-byte type.
        const std::array<uint8_t, 4> resp{0x00, 0x02,  // payload length = 2
                                          static_cast<uint8_t>((msg_type >> 8) & 0xFF),
                                          static_cast<uint8_t>(msg_type & 0xFF)};
        write_all(fd, resp.data(), resp.size());
        // After OPEN_GROUPCON, the main connection stays open for GROUP_PACKET.
        continue;
      }

      if (msg_type == EIB_GROUP_PACKET && payload_len >= 4) {
        // Group packet: [dst_addr:2][apdu:N]
        uint16_t dst_addr = static_cast<uint16_t>((payload[2] << 8) | payload[3]);
        std::vector<uint8_t> apdu(payload.begin() + 4, payload.end());
        {
          std::lock_guard<std::mutex> lock(recv_mutex_);
          received_packets_.push_back({dst_addr, std::move(apdu)});
        }
        // send_group_packet() returns after write_all() succeeds, so we don't
        // need to send a response. Just continue.
        continue;
      }

      if (msg_type == EIB_CACHE_READ || msg_type == EIB_CACHE_READ_NOWAIT) {
        // Cache read request: [addr:2]
        // Response for cache miss: [src:2][dst:2] = 4 bytes of data after type.
        // Total payload = type(2) + src(2) + dst(2) = 6 bytes.
        if (payload_len >= 2) {
          const std::array<uint8_t, 8> resp{0x00,
                                            0x06,  // payload length = 6
                                            static_cast<uint8_t>((msg_type >> 8) & 0xFF),
                                            static_cast<uint8_t>(msg_type & 0xFF),
                                            payload[0],   // src hi (echo of requested addr)
                                            payload[1],   // src lo
                                            payload[0],   // dst hi (same addr)
                                            payload[1]};  // dst lo
          write_all(fd, resp.data(), resp.size());
        }
        continue;
      }

      if (msg_type == EIB_CACHE_LAST_UPDATES_2) {
        // Cache last updates — this is the long-poll query on the cache connection.
        // For the concurrency test, we intentionally DO NOT respond,
        // simulating a long-poll that hasn't received updates yet.
        // The client should time out based on its own deadline.
        // Just hold the connection open without responding.
        //
        // Read more messages until the client closes the connection.
        while (!shutdown_.load()) {
          std::array<uint8_t, 2> dummy{};
          if (!read_exact(fd, dummy.data(), 2)) {
            break;
          }
          uint16_t pl = static_cast<uint16_t>((dummy[0] << 8) | dummy[1]);
          std::vector<uint8_t> p(pl);
          if (pl > 0 && !read_exact(fd, p.data(), pl)) {
            break;
          }
        }
        break;
      }

      // Unknown message — close
      break;
    }

    ::close(fd);
  }

  void accept_loop() {
    while (!shutdown_.load()) {
      int client = ::accept(listen_fd_, nullptr, nullptr);
      if (client < 0) {
        if (shutdown_.load()) {
          break;
        }
        continue;
      }
      // Handle each client in a separate thread (detached)
      std::thread(&FakeKnxdServer::handle_client, this, client).detach();
    }
  }

  int listen_fd_ = -1;
  std::string socket_path_;
  std::atomic<bool> shutdown_{false};
  std::thread accept_thread_;
  mutable std::mutex recv_mutex_;
  std::vector<ReceivedGroupPacket> received_packets_;
};

}  // namespace

/// FIXTURE: sets up a FakeKnxdServer and a KnxdClient connected to it.
class KnxdClientConcurrencyTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Skip if socket creation failed (e.g., no /tmp access)
    if (!fake_knxd_.is_ready()) {
      GTEST_SKIP() << "Cannot create fake knxd socket";
    }

    ASSERT_TRUE(client_.connect(fake_knxd_.path()));
    ASSERT_TRUE(client_.open_group_socket(false));
    client_.set_nonblocking(true);
  }

  void TearDown() override { client_.disconnect(); }

  FakeKnxdServer fake_knxd_;  // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes)
  KnxdClient client_;         // NOLINT(cppcoreguidelines-non-private-member-variables-in-classes)
};

/// Verify that send_group_packet() and cache_last_updates_2() are correctly
/// serialized when sharing the same mutex.  With the unified connection
/// architecture (cache operations use the main fd instead of a separate
/// cache connection), both operations share impl_->mutex.  In the fork-based
/// worker model this is fine — each worker handles one request at a time.
TEST_F(KnxdClientConcurrencyTest, SendGroupPacketNotBlockedByLongPoll) {
  std::atomic<bool> longpoll_started{false};
  std::atomic<bool> longpoll_done{false};

  // Thread A: start a long-poll that will block (no response from fake knxd)
  std::thread longpoll_thread([&]() {
    longpoll_started.store(true);
    auto result = client_.cache_last_updates_2(0, 1);  // 1 second timeout
    // Expected: times out because fake knxd never responds
    EXPECT_FALSE(result.has_value());
    longpoll_done.store(true);
  });

  // Wait for the long-poll to start blocking
  while (!longpoll_started.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  // Give it a moment to enter the poll() call
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Thread B (main): try to send a group packet while the long-poll is active.
  // With the unified mutex (cache + group share one connection), send_group_packet
  // will block until cache_last_updates_2 releases the mutex (after its 1s timeout).
  auto write_start = std::chrono::steady_clock::now();
  std::vector<uint8_t> apdu = {0x00, 0x80, 0x42};
  bool write_result = client_.send_group_packet(0x0A03, apdu);
  auto write_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - write_start)
                           .count();

  EXPECT_TRUE(write_result);

  // With the unified mutex, send_group_packet must wait for the long-poll
  // to complete. The write should take at least ~800ms (the remaining
  // long-poll time after the 100ms startup delay).
  EXPECT_GT(write_elapsed, 500) << "send_group_packet completed too quickly (" << write_elapsed
                                << "ms) — mutex serialization may not be working";

  longpoll_thread.join();
  EXPECT_TRUE(longpoll_done.load());
}

/// Verify that send_group_packet() actually sends a correctly formatted
/// group packet that the knxd server receives.  This is a regression test
/// for the "halve knxd connections" change where cache_read was moved to
/// share the main connection fd.
TEST_F(KnxdClientConcurrencyTest, SendGroupPacketReachesServer) {
  std::vector<uint8_t> apdu = {0x00, 0x80, 0x42};
  EXPECT_TRUE(client_.send_group_packet(0x0A03, apdu));

  // Give the fake server thread time to process
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto packets = fake_knxd_.received_packets();
  ASSERT_EQ(packets.size(), 1);
  EXPECT_EQ(packets[0].group_addr, 0x0A03);
  EXPECT_EQ(packets[0].apdu, apdu);
}

/// Verify that send_group_packet() works correctly even after cache_read
/// has been called on the shared connection.  cache_read clears the shared
/// read_buffer_, which must not interfere with subsequent writes.
TEST_F(KnxdClientConcurrencyTest, SendGroupPacketAfterCacheRead) {
  // First, do a cache_read to exercise the shared buffer path
  auto cache_result = client_.cache_read(0x0A03, true);
  // Fake server responds with cache miss — expected
  EXPECT_FALSE(cache_result.has_value());

  // Now send a group packet
  std::vector<uint8_t> apdu = {0x00, 0x80, 0x0C, 0x6F};
  EXPECT_TRUE(client_.send_group_packet(0x0B04, apdu));

  // Give the fake server time to process
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto packets = fake_knxd_.received_packets();
  ASSERT_EQ(packets.size(), 1);
  EXPECT_EQ(packets[0].group_addr, 0x0B04);
  EXPECT_EQ(packets[0].apdu, apdu);
}

/// Verify that cache_read followed by send_group_packet works when
/// the cache response arrives with extra data in the socket buffer.
/// cache_read clears impl_->read_buffer_ at the start, which must
/// not discard data that is still in the kernel socket buffer.
TEST_F(KnxdClientConcurrencyTest, SendGroupPacketAfterCacheReadTwice) {
  // First cache_read
  EXPECT_FALSE(client_.cache_read(0x0A03, true).has_value());

  // Send a packet
  std::vector<uint8_t> apdu1 = {0x00, 0x80, 0x42};
  EXPECT_TRUE(client_.send_group_packet(0x0A03, apdu1));

  // Second cache_read
  EXPECT_FALSE(client_.cache_read(0x0B04, true).has_value());

  // Send another packet
  std::vector<uint8_t> apdu2 = {0x00, 0x80, 0x0C, 0x6F};
  EXPECT_TRUE(client_.send_group_packet(0x0C05, apdu2));

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto packets = fake_knxd_.received_packets();
  ASSERT_EQ(packets.size(), 2);
  EXPECT_EQ(packets[0].group_addr, 0x0A03);
  EXPECT_EQ(packets[0].apdu, apdu1);
  EXPECT_EQ(packets[1].group_addr, 0x0C05);
  EXPECT_EQ(packets[1].apdu, apdu2);
}
