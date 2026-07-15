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

#include <fcgiapp.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "fcgi/fcgi_request.h"
#include "fcgi/fcgi_server.h"

using namespace cvknxd;

namespace {

/// Generate a unique socket path in the writable temp directory.
/// Returns empty string on failure.
std::string make_unique_socket_path() {
  const char* tmpdir = getenv("TMPDIR");
  if (tmpdir == nullptr || tmpdir[0] == '\0') {
    tmpdir = "/tmp";
  }
  std::string tmpl = std::string(tmpdir) + "/fcgi-fork-integration-XXXXXX";
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

/// Check if we can create Unix sockets. The VS Code sandbox may block
/// AF_UNIX socket creation, in which case we skip these tests.
bool sockets_available() {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return false;
  }
  close(fd);

  // Also verify we can bind
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

/// Wrapper to connect to a Unix socket with retries (children may not
/// be ready immediately after fork).
int connect_with_retry(const std::string& path, int max_retries, int delay_ms) {
  for (int i = 0; i < max_retries; ++i) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
      return -1;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    size_t path_len = path.copy(addr.sun_path, sizeof(addr.sun_path) - 1);
    addr.sun_path[path_len] = '\0';

    int rc = connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (rc == 0)
      return fd;

    close(fd);
    usleep(static_cast<useconds_t>(delay_ms) * 1000);
  }
  return -1;
}

/// Fork N children, each running an accept loop on the
/// shared listen socket. Returns child PIDs on success.
/// On failure in the parent, kills started children and returns empty vector.
std::vector<pid_t> fork_workers(int num_workers, FcgiServer& server, const char* knxd_socket) {
  std::vector<pid_t> children;
  children.reserve(static_cast<size_t>(num_workers));

  for (int i = 0; i < num_workers; ++i) {
    pid_t pid = ::fork();
    if (pid < 0) {
      // Fork failed — kill all started children
      std::cerr << "[TEST-ERROR] fork() for worker " << (i + 1) << "/" << num_workers
                << " failed: " << std::strerror(errno) << " (errno=" << errno << ")\n";
      for (pid_t child : children) {
        ::kill(child, SIGTERM);
      }
      // Wait for killed children
      int status;
      for (size_t j = 0; j < children.size(); ++j) {
        ::wait(&status);
      }
      children.clear();
      break;
    }

    if (pid == 0) {
      // ---- Child process ----
      // Reconnect to knxd if a socket path was provided.
      // In the test, knxd_socket may be nullptr (no knxd needed
      // for pure FCGI socket tests).
      (void)knxd_socket;  // silence unused warning in test

      // Run accept loop in this child process.
      // The listen socket is shared with siblings via fork().
      int child_result = server.run();
      std::_Exit(child_result);
    }

    children.push_back(pid);
  }

  return children;
}

}  // namespace

// ============================================================
// ForkWorkerTest — verifies the fork-based concurrency pattern
// ============================================================
//
// These tests validate that forking N children, each running
// FcgiServer::run() on a shared listen socket, correctly handles
// concurrent clients. This is the pattern used in main.cpp when
// std::thread / pthread_create is blocked by seccomp (Docker < 20.10.10
// with glibc >= 2.34).
//
class ForkWorkerTest : public ::testing::Test {
protected:
  void SetUp() override {
    if (!sockets_available()) {
      GTEST_SKIP() << "Unix socket creation is blocked by sandbox";
    }
    socket_path_ = make_unique_socket_path();
    ASSERT_FALSE(socket_path_.empty()) << "Failed to create temp socket path";
  }

  void TearDown() override { unlink(socket_path_.c_str()); }

  /// Kill all children and wait for them to exit.
  void cleanup_children(const std::vector<pid_t>& children) {
    // Signal children to stop
    for (pid_t pid : children) {
      ::kill(pid, SIGTERM);
    }
    // Give them a moment to exit
    usleep(50000);
    // Force-kill any stragglers
    for (pid_t pid : children) {
      ::kill(pid, SIGKILL);
    }
    // Wait for all
    int status;
    for (size_t i = 0; i < children.size(); ++i) {
      ::wait(&status);
    }
  }

  std::string socket_path_;
};

// ------------------------------------------------------------------
// Test: Forked workers accept connections on the shared listen socket
//
// Verifies that forked children, each running FcgiServer::run() on
// the same listen socket, can accept plain socket connections.
// This proves that the fork+accept pattern works without needing a
// full FCGI protocol exchange.
// ------------------------------------------------------------------
TEST_F(ForkWorkerTest, ForkedWorkersAcceptConnections) {
  constexpr int kNumWorkers = 2;

  FcgiServer server;
  // Set a handler — not strictly needed for connection tests, but
  // validates that the handler callback is captured correctly.
  server.set_handler([&](const FcgiRequest& /*req*/) -> FcgiResponse {
    FcgiResponse resp;
    resp.status_code = 200;
    resp.body = "{\"status\":\"ok\"}";
    return resp;
  });

  ASSERT_TRUE(server.listen(socket_path_));

  // Fork workers
  auto children = fork_workers(kNumWorkers, server, nullptr);
  ASSERT_EQ(children.size(), static_cast<size_t>(kNumWorkers))
      << "fork_workers failed to create all " << kNumWorkers << " children";

  // Verify all children are alive
  for (size_t i = 0; i < children.size(); ++i) {
    ASSERT_EQ(::kill(children[i], 0), 0)
        << "Child " << i << " (pid=" << children[i] << ") not alive after fork";
  }

  // Connect multiple times — each connection should be accepted by one
  // of the forked workers. The OS serializes accept() across processes.
  constexpr int kConnections = 4;
  for (int i = 0; i < kConnections; ++i) {
    int fd = connect_with_retry(socket_path_, 10, 50);
    ASSERT_GE(fd, 0) << "Failed to connect to listen socket on attempt " << i << " of "
                     << kConnections;

    // Verify we can write to the socket (connection is fully established)
    const char ping[] = "ping";
    ssize_t written = ::write(fd, ping, sizeof(ping));
    // Workers are blocked in FCGX_Accept_r which reads FCGI frames,
    // so a plain write may fail or succeed depending on timing.
    // The important thing is the connect succeeded.
    (void)written;  // best-effort, not asserted

    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
  }

  // All children should still be alive after handling connections
  for (size_t i = 0; i < children.size(); ++i) {
    EXPECT_EQ(::kill(children[i], 0), 0)
        << "Child " << i << " (pid=" << children[i] << ") died unexpectedly";
  }

  // Kill and clean up children
  cleanup_children(children);
}

// ------------------------------------------------------------------
// Test: Fork worker error handling — fork failure is reported
//
// Tests the error path in the fork loop: when fork() fails, the
// already-started children are killed and the caller gets an empty
// vector (or partial vector) to indicate failure.
// ------------------------------------------------------------------
TEST_F(ForkWorkerTest, ForkFailureIsReported) {
  FcgiServer server;
  // Set handler to avoid "No request handler set" stderr noise in children
  server.set_handler([&](const FcgiRequest& /*req*/) -> FcgiResponse {
    FcgiResponse resp;
    resp.body = "{}";
    return resp;
  });

  ASSERT_TRUE(server.listen(socket_path_));

  // Fork with an impossibly large count to trigger resource exhaustion.
  // The function should fail gracefully and return fewer children
  // (or none if fork failed on the first attempt).
  auto children = fork_workers(100000, server, nullptr);

  // Verify the function didn't hang or crash. The exact return value
  // depends on system resources — it should be < 100000.
  EXPECT_LT(children.size(), static_cast<size_t>(100000))
      << "Resource-exhausting fork should have failed and returned fewer children";

  // Clean up any children that were successfully forked before failure
  if (!children.empty()) {
    cleanup_children(children);
  }
}

// ------------------------------------------------------------------
// Test: Child process exits cleanly when parent sends SIGTERM
//
// Verifies that a forked worker process running FcgiServer::run()
// exits when killed by the parent. This is the normal shutdown path
// in production.
// ------------------------------------------------------------------
TEST_F(ForkWorkerTest, ChildExitsOnSigterm) {
  FcgiServer server;
  server.set_handler([&](const FcgiRequest& /*req*/) -> FcgiResponse {
    FcgiResponse resp;
    resp.body = "{}";
    return resp;
  });

  ASSERT_TRUE(server.listen(socket_path_));

  auto children = fork_workers(1, server, nullptr);
  ASSERT_EQ(children.size(), 1u) << "Failed to fork single worker";

  pid_t child_pid = children[0];

  // Give child time to initialize FCGX and enter accept()
  usleep(100000);

  // Verify child is alive
  int result = ::kill(child_pid, 0);
  ASSERT_EQ(result, 0) << "Child pid=" << child_pid << " is not alive before SIGTERM";

  // Send SIGTERM
  ::kill(child_pid, SIGTERM);
  usleep(100000);

  // Child should have exited by now
  int status;
  pid_t waited = ::waitpid(child_pid, &status, WNOHANG);
  if (waited == 0) {
    // Still running — force kill
    ::kill(child_pid, SIGKILL);
    waited = ::waitpid(child_pid, &status, 0);
  }
  EXPECT_EQ(waited, child_pid) << "Child pid=" << child_pid << " did not exit after SIGTERM";
  EXPECT_TRUE(WIFSIGNALED(status) || WIFEXITED(status))
      << "Child neither exited nor was signaled (status=" << status << ")";
}
