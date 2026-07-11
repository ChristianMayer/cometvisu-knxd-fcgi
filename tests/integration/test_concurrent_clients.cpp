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

#include <fastcgi.h>
#include <fcgiapp.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "fcgi/fcgi_server.h"

using namespace cvknxd;

namespace {

/// Generate a unique socket path in the writable temp directory.
std::string make_unique_socket_path() {
  const char* tmpdir = getenv("TMPDIR");
  if (tmpdir == nullptr || tmpdir[0] == '\0') {
    tmpdir = "/tmp";
  }
  std::string tmpl = std::string(tmpdir) + "/fcgi-concurrent-test-XXXXXX";
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

/// Check if we can create Unix sockets.
bool sockets_available() {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return false;
  }
  close(fd);
  std::string path = make_unique_socket_path();
  if (path.empty()) {
    return false;
  }
  int s = socket(AF_UNIX, SOCK_STREAM, 0);
  if (s < 0) {
    return false;
  }
  struct sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  size_t path_len = path.copy(addr.sun_path, sizeof(addr.sun_path) - 1);
  addr.sun_path[path_len] = '\0';
  int rc = bind(s, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
  close(s);
  unlink(path.c_str());
  return rc == 0;
}

// ============================================================
// Minimal FCGI test client — speaks just enough FCGI protocol
// to connect, send a request, and read the response.
// ============================================================

/// Send an FCGI record header followed by body.
/// Returns true if all bytes were written.
bool fcgi_send_record(int fd, uint8_t type, uint16_t request_id, const uint8_t* body,
                      uint16_t body_len) {
  uint8_t header[8];
  header[0] = FCGI_VERSION_1;                                  // version
  header[1] = type;                                            // type
  header[2] = static_cast<uint8_t>((request_id >> 8) & 0xFF);  // request ID B1
  header[3] = static_cast<uint8_t>(request_id & 0xFF);         // request ID B0
  header[4] = static_cast<uint8_t>((body_len >> 8) & 0xFF);    // content length B1
  header[5] = static_cast<uint8_t>(body_len & 0xFF);           // content length B0
  header[6] = 0;                                               // padding length
  header[7] = 0;                                               // reserved

  // Write header
  size_t off = 0;
  while (off < sizeof(header)) {
    ssize_t n = write(fd, header + off, sizeof(header) - off);
    if (n <= 0)
      return false;
    off += static_cast<size_t>(n);
  }

  // Write body
  if (body != nullptr && body_len > 0) {
    off = 0;
    while (off < body_len) {
      ssize_t n = write(fd, body + off, body_len - off);
      if (n <= 0)
        return false;
      off += static_cast<size_t>(n);
    }
  }

  return true;
}

/// Read exactly N bytes from a file descriptor. Returns true on success.
bool read_exact(int fd, uint8_t* buf, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = read(fd, buf + off, len - off);
    if (n <= 0)
      return false;
    off += static_cast<size_t>(n);
  }
  return true;
}

/// Read an FCGI record header. Returns false on EOF/error.
bool fcgi_read_header(int fd, uint8_t& out_type, uint16_t& out_request_id,
                      uint16_t& out_content_len) {
  uint8_t header[8];
  if (!read_exact(fd, header, 8))
    return false;

  out_type = header[1];
  out_request_id = static_cast<uint16_t>((header[2] << 8) | header[3]);
  out_content_len = static_cast<uint16_t>((header[4] << 8) | header[5]);
  uint8_t padding = header[6];

  // Read and discard padding
  if (padding > 0) {
    uint8_t dummy[256];
    if (!read_exact(fd, dummy, padding))
      return false;
  }
  return true;
}

/// Read an FCGI record body.
bool fcgi_read_body(int fd, std::vector<uint8_t>& out_body, uint16_t content_len) {
  out_body.resize(content_len);
  if (content_len == 0)
    return true;
  return read_exact(fd, out_body.data(), content_len);
}

/// Encode FCGI name-value pair.
void encode_nv_pair(std::vector<uint8_t>& buf, const std::string& name, const std::string& value) {
  // Name length encoding
  if (name.size() < 128) {
    buf.push_back(static_cast<uint8_t>(name.size()));
  } else {
    buf.push_back(static_cast<uint8_t>((name.size() >> 24) | 0x80));
    buf.push_back(static_cast<uint8_t>((name.size() >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((name.size() >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(name.size() & 0xFF));
  }
  // Value length encoding
  if (value.size() < 128) {
    buf.push_back(static_cast<uint8_t>(value.size()));
  } else {
    buf.push_back(static_cast<uint8_t>((value.size() >> 24) | 0x80));
    buf.push_back(static_cast<uint8_t>((value.size() >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((value.size() >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(value.size() & 0xFF));
  }
  buf.insert(buf.end(), name.begin(), name.end());
  buf.insert(buf.end(), value.begin(), value.end());
}

/// A minimal FCGI client that connects to a Unix socket, sends a GET request,
/// and reads the response body.
class FcgiTestClient {
public:
  ~FcgiTestClient() { disconnect(); }

  [[nodiscard]] bool connect(const std::string& socket_path) {
    fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0)
      return false;

    // Set a receive timeout to prevent test hangs in CI.
    // If the server doesn't respond within 5 seconds, read_response() fails.
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    if (::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
      close(fd_);
      fd_ = -1;
      return false;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    size_t path_len = socket_path.copy(addr.sun_path, sizeof(addr.sun_path) - 1);
    addr.sun_path[path_len] = '\0';

    if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
      close(fd_);
      fd_ = -1;
      return false;
    }
    return true;
  }

  void disconnect() {
    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }
  }

  /// Send a GET /r request with the given query string.
  /// @param request_id FCGI request ID (must be unique per connection).
  /// @param query_string The QUERY_STRING to send (e.g. "a=KNX:1/2/3&t=5").
  /// @param path_info The PATH_INFO (e.g. "/r").
  /// @return true if the request was sent successfully.
  [[nodiscard]] bool send_get_request(uint16_t request_id, const std::string& query_string,
                                      const std::string& path_info) {
    // --- BEGIN_REQUEST ---
    uint8_t begin_body[8] = {};
    begin_body[0] = 0;  // role = FCGI_RESPONDER (high byte)
    begin_body[1] = 1;  // role = FCGI_RESPONDER (low byte)
    begin_body[2] = 0;  // flags = 0 (don't keep connection)
    if (!fcgi_send_record(fd_, FCGI_BEGIN_REQUEST, request_id, begin_body, 8)) {
      return false;
    }

    // --- PARAMS ---
    std::vector<uint8_t> params;
    encode_nv_pair(params, "REQUEST_METHOD", "GET");
    encode_nv_pair(params, "QUERY_STRING", query_string);
    encode_nv_pair(params, "PATH_INFO", path_info);
    encode_nv_pair(params, "SCRIPT_NAME", "/cgi-bin");

    if (!fcgi_send_record(fd_, FCGI_PARAMS, request_id, params.data(),
                          static_cast<uint16_t>(params.size()))) {
      return false;
    }
    // End of params: empty FCGI_PARAMS record
    if (!fcgi_send_record(fd_, FCGI_PARAMS, request_id, nullptr, 0)) {
      return false;
    }

    return true;
  }

  /// Read the response from the server.
  /// Collects all FCGI_STDOUT content and returns it as a string.
  /// @param request_id The expected FCGI request ID.
  /// @return The response body, or empty string on error.
  [[nodiscard]] std::string read_response(uint16_t request_id) {
    std::string body;

    while (true) {
      uint8_t type;
      uint16_t resp_id, content_len;
      if (!fcgi_read_header(fd_, type, resp_id, content_len)) {
        return "";
      }

      if (resp_id != request_id)
        continue;  // skip other request IDs

      std::vector<uint8_t> content;
      if (!fcgi_read_body(fd_, content, content_len)) {
        return "";
      }

      if (type == FCGI_STDOUT) {
        if (!content.empty()) {
          body.append(reinterpret_cast<const char*>(content.data()), content.size());
        }
      } else if (type == FCGI_END_REQUEST) {
        break;  // done
      }
    }

    return body;
  }

private:
  int fd_ = -1;
};

}  // namespace

// ============================================================
// Concurrent client tests
// ============================================================

class ConcurrentClientsTest : public ::testing::Test {
protected:
  void SetUp() override {
    if (!sockets_available()) {
      GTEST_SKIP() << "Unix socket creation blocked by sandbox";
    }
    socket_path_ = make_unique_socket_path();
    ASSERT_FALSE(socket_path_.empty()) << "Failed to create temp socket path";
  }

  void TearDown() override { unlink(socket_path_.c_str()); }

  std::string socket_path_;
};

/// Test: Multiple concurrent clients are processed independently.
///
/// Without multithreading, if client A does a long-poll (simulated by
/// a 300ms handler delay), client B would have to wait 300ms before
/// even being accepted. With multithreading, both should be processed
/// concurrently — total time should be close to the individual handler
/// time (300ms), not N * 300ms.
///
/// This test uses the multithreaded API that doesn't exist yet → RED.
TEST_F(ConcurrentClientsTest, MultipleClientsProcessedConcurrently) {
  // Track concurrency: how many handler invocations are active simultaneously
  std::atomic<int> concurrent_count{0};
  std::atomic<int> max_concurrent{0};

  constexpr int kNumClients = 3;
  constexpr int kHandlerDelayMs = 50;
  constexpr int kNumThreads = 4;

  // Create server with a handler that simulates long-poll delay
  FcgiServer server;
  ASSERT_TRUE(server.listen(socket_path_));
  ASSERT_TRUE(server.is_listening());

  server.set_handler([&]([[maybe_unused]] const FcgiRequest& req) -> FcgiResponse {
    int current = concurrent_count.fetch_add(1) + 1;
    // Update max concurrent
    int prev_max = max_concurrent.load();
    while (current > prev_max && !max_concurrent.compare_exchange_weak(prev_max, current)) {
      prev_max = max_concurrent.load();
    }

    // Simulate long-poll delay
    std::this_thread::sleep_for(std::chrono::milliseconds(kHandlerDelayMs));

    concurrent_count.fetch_sub(1);

    FcgiResponse resp;
    resp.status_code = 200;
    resp.body = "{\"status\":\"ok\"}";
    return resp;
  });

  // Start server in multithreaded mode in a background thread.
  // Use std::promise/future so we can wait with a timeout — blocking
  // join() would hang CI forever if shutdown doesn't unblock workers.
  std::promise<int> server_promise;
  auto server_future = server_promise.get_future();
  std::atomic<bool> server_ready{false};
  std::thread server_thread([&]() {
    server_ready.store(true);
    int result = server.run_multithreaded(kNumThreads);
    server_promise.set_value(result);
  });

  // Wait for server to be ready
  while (!server_ready.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  // Give the server a moment to start listening
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Spawn concurrent client threads
  std::vector<std::thread> client_threads;
  std::vector<std::string> results(kNumClients);
  std::vector<bool> success(kNumClients, false);

  auto t_start = std::chrono::steady_clock::now();

  for (int i = 0; i < kNumClients; ++i) {
    client_threads.emplace_back([&, i, this]() {
      FcgiTestClient client;
      if (!client.connect(socket_path_)) {
        return;
      }

      // Each client does a /r long-poll request
      std::string qs = "a=KNX:1/2/3&t=30&client=" + std::to_string(i);
      if (!client.send_get_request(static_cast<uint16_t>(i + 1), qs, "/r")) {
        return;
      }

      results[i] = client.read_response(static_cast<uint16_t>(i + 1));
      success[i] = !results[i].empty();
    });
  }

  // Join all client threads
  for (auto& t : client_threads) {
    t.join();
  }

  auto t_end = std::chrono::steady_clock::now();
  auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();

  // Verify all clients got responses
  for (int i = 0; i < kNumClients; ++i) {
    EXPECT_TRUE(success[i]) << "Client " << i << " failed to get response";
    if (success[i]) {
      EXPECT_NE(results[i].find("{\"status\":\"ok\"}"), std::string::npos)
          << "Client " << i << " got unexpected response: " << results[i];
    }
  }

  // Verify concurrency: at least 2 handlers were active simultaneously
  EXPECT_GE(max_concurrent.load(), 2)
      << "Expected at least 2 concurrent handlers, but max was " << max_concurrent.load();

  // Verify timing: with N threads and N clients, total time should be
  // close to kHandlerDelayMs, not kNumClients * kHandlerDelayMs.
  // Allow overhead for thread startup + FCGI protocol.
  // Sequential would be ~150ms (3×50ms); concurrent should be < 400ms.
  EXPECT_LT(total_ms, 400) << "Total time " << total_ms
                           << "ms suggests sequential processing or hang "
                           << "(expected < 400ms with concurrent threads)";

  // Shutdown: unblock the accept loops and wait for workers.
  server.shutdown();

  // Wait for the server thread with a firm 15-second timeout.
  // If shutdown() works correctly, this returns in < 1 second.
  // If workers are stuck in accept(), the future times out and we
  // detach the thread — the test reports failure instead of hanging CI.
  auto status = server_future.wait_for(std::chrono::seconds(15));
  if (status == std::future_status::timeout) {
    server_thread.detach();
    FAIL() << "Server thread did not exit within 15 seconds after shutdown(). "
           << "Workers likely stuck in accept() — shutdown(listen_fd, SHUT_RDWR) "
           << "may not be supported on this platform.";
  } else {
    server_thread.join();
    int result = server_future.get();
    EXPECT_EQ(result, 0) << "server.run_multithreaded() returned error";
  }
}
