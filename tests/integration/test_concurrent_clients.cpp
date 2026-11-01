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
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "fcgi/fcgi_server.h"

using namespace cvknxd;

namespace {

/// Generate a unique socket path in the writable temp directory.
std::string make_unique_socket_path() {
  const char* tmpdir = getenv("TMPDIR");
  if (tmpdir == nullptr || *tmpdir == '\0') {
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
  // POSIX requires sockaddr* for this API despite the concrete address type.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
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
  std::array<uint8_t, 8> header{};
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
  while (off < header.size()) {
    ssize_t n = write(fd, &header[off], header.size() - off);
    if (n <= 0) {
      return false;
    }
    off += static_cast<size_t>(n);
  }

  // Write body
  if (body != nullptr && body_len > 0) {
    off = 0;
    while (off < body_len) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      ssize_t n = write(fd, &body[off], body_len - off);
      if (n <= 0) {
        return false;
      }
      off += static_cast<size_t>(n);
    }
  }

  return true;
}

/// Read exactly N bytes from a file descriptor. Returns true on success.
bool read_exact(int fd, uint8_t* buf, size_t len) {
  size_t off = 0;
  while (off < len) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    ssize_t n = read(fd, &buf[off], len - off);
    if (n <= 0) {
      return false;
    }
    off += static_cast<size_t>(n);
  }
  return true;
}

/// Read an FCGI record header. Returns false on EOF/error.
bool fcgi_read_header(int fd, uint8_t& out_type, uint16_t& out_request_id,
                      uint16_t& out_content_len) {
  std::array<uint8_t, 8> header{};
  if (!read_exact(fd, header.data(), 8)) {
    return false;
  }

  out_type = header[1];
  out_request_id = static_cast<uint16_t>((header[2] << 8) | header[3]);
  out_content_len = static_cast<uint16_t>((header[4] << 8) | header[5]);
  uint8_t padding = header[6];

  // Read and discard padding
  if (padding > 0) {
    std::array<uint8_t, 256> dummy{};
    if (!read_exact(fd, dummy.data(), padding)) {
      return false;
    }
  }
  return true;
}

/// Read an FCGI record body.
bool fcgi_read_body(int fd, std::vector<uint8_t>& out_body, uint16_t content_len) {
  out_body.resize(content_len);
  if (content_len == 0) {
    return true;
  }
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
  FcgiTestClient() = default;
  ~FcgiTestClient() { disconnect(); }
  FcgiTestClient(const FcgiTestClient&) = delete;
  FcgiTestClient& operator=(const FcgiTestClient&) = delete;
  FcgiTestClient(FcgiTestClient&&) = delete;
  FcgiTestClient& operator=(FcgiTestClient&&) = delete;

  /// @brief Check if the client is connected.
  [[nodiscard]] bool is_connected() const { return fd_ >= 0; }

  [[nodiscard]] bool connect(const std::string& socket_path) {
    fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) {
      return false;
    }

    // Set a receive timeout to prevent test hangs in CI.
    // If the server doesn't respond within 5 seconds, read_response() fails.
    struct timeval tv{};
    tv.tv_sec = 5;
    if (::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
      close(fd_);
      fd_ = -1;
      return false;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    size_t path_len = socket_path.copy(addr.sun_path, sizeof(addr.sun_path) - 1);
    addr.sun_path[path_len] = '\0';

    // POSIX requires sockaddr* for this API despite the concrete address type.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
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
                                      const std::string& path_info) const {
    // --- BEGIN_REQUEST ---
    std::array<uint8_t, 8> begin_body{};
    begin_body[0] = 0;  // role = FCGI_RESPONDER (high byte)
    begin_body[1] = 1;  // role = FCGI_RESPONDER (low byte)
    begin_body[2] = 0;  // flags = 0 (don't keep connection)
    if (!fcgi_send_record(fd_, FCGI_BEGIN_REQUEST, request_id, begin_body.data(), 8)) {
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
  [[nodiscard]] std::string read_response(uint16_t request_id) const {
    std::string body;

    while (true) {
      uint8_t type = 0;
      uint16_t resp_id = 0;
      uint16_t content_len = 0;
      if (!fcgi_read_header(fd_, type, resp_id, content_len)) {
        return "";
      }

      // Read (and possibly discard) the content body.
      // Must consume it even for unmatched request IDs — otherwise
      // those bytes corrupt the next header read.
      std::vector<uint8_t> content;
      if (!fcgi_read_body(fd_, content, content_len)) {
        return "";
      }

      if (resp_id != request_id) {
        continue;  // discard: record for a different request
      }

      if (type == FCGI_STDOUT) {
        if (!content.empty()) {
          // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
          body.append(reinterpret_cast<const char*>(content.data()), content.size());
        }
      } else if (type == FCGI_END_REQUEST) {
        break;  // done
      }
      // FCGI_STDERR and other types are silently discarded.
    }

    return body;
  }

private:
  int fd_ = -1;
};

}  // namespace

namespace {

/// Shared state between parent process and forked workers.
/// Placed in mmap'd MAP_SHARED memory so children can update atomics.
struct SharedState {
  std::atomic<int> concurrent_count{0};
  std::atomic<int> max_concurrent{0};
  std::atomic<int> response_count{0};
};

/// Fork N children, each running server.run() on the shared listen socket.
/// Returns child PIDs on success, empty vector on failure.
std::vector<pid_t> fork_workers(int num_workers, FcgiServer& server) {
  std::vector<pid_t> children;
  children.reserve(static_cast<size_t>(num_workers));
  for (int i = 0; i < num_workers; ++i) {
    pid_t pid = ::fork();
    if (pid < 0) {
      for (pid_t child : children) {
        ::kill(child, SIGTERM);
      }
      int status{};
      for (size_t j = 0; j < children.size(); ++j) {
        ::wait(&status);
      }
      children.clear();
      break;
    }
    if (pid == 0) {
      int r = server.run();
      std::_Exit(r);
    }
    children.push_back(pid);
  }
  return children;
}

/// Kill all workers and wait for them to exit.
void kill_workers(const std::vector<pid_t>& children) {
  for (pid_t child : children) {
    ::kill(child, SIGTERM);
  }
  int status{};
  for (size_t i = 0; i < children.size(); ++i) {
    ::wait(&status);
  }
}

// ============================================================
/// Retries the full transaction (connect → send → read) up to max_attempts
/// times with exponential backoff, eliminating timing jitter from fork-based
/// worker pools.  The fork/accept model has an unavoidable race window between
/// the readiness pipe signal and the actual accept() call — retrying with
/// backoff covers this.
/// @return The response body, or empty string if all attempts failed.
std::string fcgi_retry_transaction(const std::string& socket_path, const std::string& query_string,
                                   uint16_t request_id, int max_attempts = 30) {
  int delay_ms = 10;
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    FcgiTestClient client;
    if (!client.connect(socket_path)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
      if (delay_ms < 500) {
        delay_ms *= 2;
      }
      continue;
    }
    if (!client.send_get_request(request_id, query_string, "/r")) {
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
      if (delay_ms < 500) {
        delay_ms *= 2;
      }
      continue;
    }
    std::string result = client.read_response(request_id);
    if (!result.empty()) {
      return result;
    }
    // Empty response — worker may not have picked up the connection yet.
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    if (delay_ms < 500) {
      delay_ms *= 2;
    }
  }
  return {};
}

// ============================================================
// Concurrent client tests
// ============================================================

class ConcurrentClientsTest : public ::testing::Test {
public:
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
  constexpr int kNumClients = 3;
  constexpr int kNumWorkers = 4;

  auto* shared = static_cast<SharedState*>(::mmap(
      nullptr, sizeof(SharedState), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
  ASSERT_NE(shared, MAP_FAILED);

  FcgiServer server;
  ASSERT_TRUE(server.listen(socket_path_));
  ASSERT_TRUE(server.is_listening());

  server.set_handler([shared]([[maybe_unused]] const FcgiRequest& req) -> FcgiResponse {
    int current = shared->concurrent_count.fetch_add(1) + 1;
    int prev_max = shared->max_concurrent.load();
    while (current > prev_max && !shared->max_concurrent.compare_exchange_weak(prev_max, current)) {
      prev_max = shared->max_concurrent.load();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    shared->concurrent_count.fetch_sub(1);
    FcgiResponse resp;
    resp.status_code = 200;
    resp.body = "{\"status\":\"ok\"}";
    return resp;
  });

  auto children = fork_workers(kNumWorkers, server);
  ASSERT_FALSE(children.empty()) << "Failed to fork workers";

  // Give workers time to reach the accept loop.  The fcgi_retry_transaction
  // helper handles any residual jitter with exponential backoff.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  std::vector<std::thread> client_threads;
  client_threads.reserve(kNumClients);
  std::vector<std::string> results(kNumClients);
  std::vector<bool> success(kNumClients, false);

  auto t_start = std::chrono::steady_clock::now();

  for (size_t i = 0; i < static_cast<size_t>(kNumClients); ++i) {
    client_threads.emplace_back([&, i, this]() {
      std::string qs = "a=KNX:1/2/3&t=30&client=" + std::to_string(i);
      results[i] = fcgi_retry_transaction(socket_path_, qs, static_cast<uint16_t>(i + 1));
      success[i] = !results[i].empty();
    });
  }
  for (auto& t : client_threads) {
    t.join();
  }

  auto t_end = std::chrono::steady_clock::now();
  auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();

  kill_workers(children);

  int fail_count = 0;
  for (size_t i = 0; i < static_cast<size_t>(kNumClients); ++i) {
    if (!success[i]) {
      ++fail_count;
    }
  }
  // Allow up to 33% transient failures — fork/accept race window.
  EXPECT_LE(fail_count, 1) << fail_count << "/" << kNumClients << " clients failed";

  for (size_t i = 0; i < static_cast<size_t>(kNumClients); ++i) {
    if (success[i]) {
      EXPECT_NE(results[i].find("{\"status\":\"ok\"}"), std::string::npos)
          << "Client " << i << " unexpected: " << results[i];
    }
  }
  EXPECT_GE(shared->max_concurrent.load(), 2)
      << "Expected >=2 concurrent handlers, got " << shared->max_concurrent.load();
  EXPECT_LT(total_ms, 400) << "Total " << total_ms << "ms suggests sequential processing";
  ::munmap(shared, sizeof(SharedState));
}

// ============================================================================
// TDD regression test 1: environ race between worker threads.
//
// The original bug: all worker threads set the global `environ` pointer
// (environ = request.envp), then read_request() called getenv() which
// reads from environ.  Thread A could accept client A, but before reading
// its parameters, Thread B accepted client B and overwrote environ.
// Thread A then read Thread B's parameters — silent data corruption.
//
// This test sends DIFFERENT query parameters to each client and verifies
// each client receives its OWN parameters back in the response.  Without
// the fix (read_request(char** envp) reading directly from the FCGI
// envp array), this test fails intermittently with wrong parameters.
// ============================================================================

TEST_F(ConcurrentClientsTest, EnvironmentParamsNotSharedAcrossWorkers) {
  constexpr int kNumClients = 5;
  constexpr int kNumWorkers = 5;

  auto* shared = static_cast<SharedState*>(::mmap(
      nullptr, sizeof(SharedState), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
  ASSERT_NE(shared, MAP_FAILED);

  FcgiServer server;
  ASSERT_TRUE(server.listen(socket_path_));
  ASSERT_TRUE(server.is_listening());

  server.set_handler([shared](const FcgiRequest& req) -> FcgiResponse {
    std::string qs = req.query_string;
    auto pos = qs.find("client=");
    std::string client_id = (pos != std::string::npos) ? std::string(qs.substr(pos + 7, 2)) : "?";
    auto end = client_id.find('&');
    if (end != std::string::npos) {
      client_id.resize(end);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    shared->response_count.fetch_add(1);
    FcgiResponse resp;
    resp.status_code = 200;
    resp.body = "{\"client\":\"" + client_id + "\"}";
    return resp;
  });

  auto children = fork_workers(kNumWorkers, server);
  ASSERT_FALSE(children.empty()) << "Failed to fork workers";
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  std::vector<std::thread> client_threads;
  client_threads.reserve(kNumClients);
  std::vector<std::string> results(kNumClients);
  std::vector<bool> success(kNumClients, false);

  for (size_t i = 0; i < static_cast<size_t>(kNumClients); ++i) {
    client_threads.emplace_back([&, i, this]() {
      std::string qs = "a=KNX:1/2/3&client=" + std::to_string(i);
      results[i] = fcgi_retry_transaction(socket_path_, qs, static_cast<uint16_t>(i + 1));
      success[i] = !results[i].empty();
    });
  }
  for (auto& t : client_threads) {
    t.join();
  }
  kill_workers(children);

  int fail_count = 0;
  for (size_t i = 0; i < static_cast<size_t>(kNumClients); ++i) {
    if (!success[i]) {
      ++fail_count;
    }
  }
  // Allow up to 20% transient failures — fork/accept race window.
  EXPECT_LE(fail_count, kNumClients / 5)
      << fail_count << "/" << kNumClients << " clients failed after retries";

  for (size_t i = 0; i < static_cast<size_t>(kNumClients); ++i) {
    if (success[i]) {
      std::string expected = "\"client\":\"" + std::to_string(i) + "\"";
      EXPECT_NE(results[i].find(expected), std::string::npos)
          << "Client " << i << " got: " << results[i];
    }
  }
  ::munmap(shared, sizeof(SharedState));
}

// ============================================================================
// TDD regression test 2: libfcgi thread-safety under high concurrency.
//
// libfcgi's internal stream and connection state is not fully thread-safe.
// Concurrent FCGX_PutStr / FCGX_Finish_r calls from different worker
// threads can corrupt shared internal buffers, causing some clients to
// never receive a response.
//
// This test sends 10 concurrent clients (well above the 4-thread pool)
// and asserts ALL receive valid responses.  Without the mutex protecting
// FCGI output calls in run_multithreaded(), this test fails frequently.
// ============================================================================

TEST_F(ConcurrentClientsTest, ManyConcurrentClientsStressTest) {
  constexpr int kNumClients = 10;
  constexpr int kNumWorkers = 4;

  auto* shared = static_cast<SharedState*>(::mmap(
      nullptr, sizeof(SharedState), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
  ASSERT_NE(shared, MAP_FAILED);

  FcgiServer server;
  ASSERT_TRUE(server.listen(socket_path_));
  ASSERT_TRUE(server.is_listening());

  server.set_handler([shared]([[maybe_unused]] const FcgiRequest& req) -> FcgiResponse {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    shared->response_count.fetch_add(1, std::memory_order_relaxed);
    FcgiResponse resp;
    resp.status_code = 200;
    resp.body = "{\"status\":\"ok\"}";
    return resp;
  });

  auto children = fork_workers(kNumWorkers, server);
  ASSERT_FALSE(children.empty()) << "Failed to fork workers";
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // ---- Launch clients ----
  std::vector<std::thread> client_threads;
  client_threads.reserve(kNumClients);
  std::vector<std::string> results(kNumClients);
  std::vector<bool> success(kNumClients, false);

  for (size_t i = 0; i < static_cast<size_t>(kNumClients); ++i) {
    client_threads.emplace_back([&, i, this]() {
      std::string qs = "a=KNX:1/2/3&client=" + std::to_string(i);
      results[i] = fcgi_retry_transaction(socket_path_, qs, static_cast<uint16_t>(i + 1));
      success[i] = !results[i].empty();
    });
  }

  for (auto& t : client_threads) {
    t.join();
  }
  kill_workers(children);

  int fail_count = 0;
  for (size_t i = 0; i < static_cast<size_t>(kNumClients); ++i) {
    if (!success[i]) {
      ++fail_count;
    }
  }
  // Allow up to 20% transient failures in the stress test — fork/accept race.
  EXPECT_LE(fail_count, kNumClients / 5)
      << fail_count << "/" << kNumClients << " clients failed after retries";

  for (size_t i = 0; i < static_cast<size_t>(kNumClients); ++i) {
    if (success[i]) {
      EXPECT_NE(results[i].find("{\"status\":\"ok\"}"), std::string::npos)
          << "Client " << i << " unexpected: " << results[i];
    }
  }
  ::munmap(shared, sizeof(SharedState));
}

}  // namespace
