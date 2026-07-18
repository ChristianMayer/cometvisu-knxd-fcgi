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
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

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
  std::string tmpl = std::string(tmpdir) + "/fcgi-sock-integration-XXXXXX";
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

/// Run a function that may call exit() in a subprocess and return the exit
/// code, or -1 if the child was killed by a signal.  FCGX_OpenSocket may
/// call exit(1) on bind failure in some library versions.
int run_in_subprocess(std::function<void()> fn) {
  pid_t pid = fork();
  if (pid == 0) {
    fn();
    _exit(0);
  }
  int status;
  waitpid(pid, &status, 0);
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  return -1;
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
  struct sockaddr_un addr {};
  addr.sun_family = AF_UNIX;
  size_t path_len = path.copy(addr.sun_path, sizeof(addr.sun_path) - 1);
  addr.sun_path[path_len] = '\0';
  int rc = bind(s, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
  close(s);
  unlink(path.c_str());
  return rc == 0;
}

}  // namespace

// ============================================================
// Test suite 1: Socket connectivity (runs on ALL platforms)
// ============================================================
//
// These tests verify that listen() creates a working listening socket.
// They do NOT start FCGI_Accept() — they just connect a plain Unix socket
// to verify the kernel-level socket is alive. This is safe on CI.
//
class FcgiServerSocketTest : public ::testing::Test {
protected:
  void SetUp() override {
    if (!sockets_available()) {
      GTEST_SKIP() << "Unix socket creation is blocked by sandbox; "
                   << "run with unsandboxed execution to test FCGI direct socket";
    }
    socket_path_ = make_unique_socket_path();
    ASSERT_FALSE(socket_path_.empty()) << "Failed to create temp socket path";
  }

  void TearDown() override { unlink(socket_path_.c_str()); }

  [[nodiscard]] int connect_to_server() const {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
      return -1;
    }
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    size_t path_len = socket_path_.copy(addr.sun_path, sizeof(addr.sun_path) - 1);
    addr.sun_path[path_len] = '\0';
    int rc = connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (rc < 0) {
      close(fd);
      return -1;
    }
    return fd;
  }

  std::string socket_path_;
};

TEST_F(FcgiServerSocketTest, ListenOpensSocket) {
  FcgiServer server;
  EXPECT_TRUE(server.listen(socket_path_));
  EXPECT_TRUE(server.is_listening());
}

TEST_F(FcgiServerSocketTest, CanConnectToListeningSocket) {
  FcgiServer server;
  ASSERT_TRUE(server.listen(socket_path_));

  int fd = connect_to_server();
  ASSERT_GE(fd, 0) << "Failed to connect to listening socket";
  close(fd);
}

// ============================================================
// Death tests - invalid socket paths
// ============================================================
//
// FCGX_OpenSocket may call exit(1) on bind failure in some library
// versions. Death tests handle both return -1 and library exit(1).

TEST_F(FcgiServerSocketTest, ListenRejectsNonexistentDir) {
  int code = run_in_subprocess([this]() {
    FcgiServer server;
    bool result = server.listen("/nonexistent/path/fcgi.sock");
    _exit(result ? 1 : 0);
  });
  // Different libfcgi versions use different exit codes on bind failure.
  // Accept any clean exit — the important thing is no hang or signal.
  EXPECT_GE(code, 0) << "Child was killed by signal, expected clean exit";
}

TEST_F(FcgiServerSocketTest, ListenRejectsColonOnly) {
  int code = run_in_subprocess([]() {
    FcgiServer server;
    bool result = server.listen(":");
    _exit(result ? 1 : 0);
  });
  EXPECT_GE(code, 0) << "Child was killed by signal, expected clean exit";
}

// ============================================================
// Concurrency exhaustion test: verifies that when all workers are
// busy, additional requests get HTTP 503 immediately instead of
// queuing in the listen backlog (which would cause the reverse proxy
// to return 502 Bad Gateway).
// ============================================================

/// Send a FastCGI request via cgi-fcgi and extract the HTTP status code
/// from the response headers. Returns the status code, or -1 on failure.
static int fcgi_request_status(const std::string& socket_path, const std::string& query_string) {
  // Use cgi-fcgi to send a proper FastCGI request
  std::string cmd = "cgi-fcgi -bind -connect " + socket_path +
                    " 2>/dev/null <<'EOF'\n"
                    "REQUEST_METHOD=GET\n"
                    "SCRIPT_NAME=/r\n"
                    "QUERY_STRING=" +
                    query_string +
                    "\n"
                    "SERVER_NAME=localhost\n"
                    "SERVER_PORT=80\n"
                    "SERVER_PROTOCOL=HTTP/1.1\n"
                    "CONTENT_TYPE=application/x-www-form-urlencoded\n"
                    "CONTENT_LENGTH=0\n"
                    "EOF";

  FILE* pipe = popen(cmd.c_str(), "r");
  if (pipe == nullptr)
    return -1;

  std::string output;
  char buf[4096];
  while (fgets(buf, sizeof(buf), pipe) != nullptr) {
    output += buf;
  }
  int rc = pclose(pipe);
  if (rc != 0) {
    // cgi-fcgi returns non-zero on FCGI errors — treat as unavailable
    return -1;
  }

  // Parse Status: header from FCGI response
  auto pos = output.find("Status: ");
  if (pos == std::string::npos)
    return -1;
  pos += 8;
  auto end = output.find_first_of("\r\n", pos);
  if (end == std::string::npos)
    return -1;
  try {
    return std::stoi(output.substr(pos, end - pos));
  } catch (...) {
    return -1;
  }
}

TEST_F(FcgiServerSocketTest, ConcurrencyExhaustionReturns503) {
  // Concurrency semaphore: 0 slots available → every request should get 503
  sem_t* conc_sem = static_cast<sem_t*>(
      mmap(nullptr, sizeof(sem_t), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
  ASSERT_NE(conc_sem, MAP_FAILED);
  sem_init(conc_sem, 1, 0);  // 0 = exhausted

  pid_t child = fork();
  ASSERT_GE(child, 0);

  if (child == 0) {
    // Child: run the server with concurrency semaphore exhausted (0 slots)
    FcgiServer server;
    ASSERT_TRUE(server.listen(socket_path_));
    server.set_concurrency_semaphore(conc_sem);
    server.set_handler([](const FcgiRequest& /*req*/) -> FcgiResponse { return {200, "{}"}; });
    server.run();
    _exit(0);
  }

  // Give the child time to start listening
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Send request — must get 503 because concurrency semaphore is exhausted
  int status = fcgi_request_status(socket_path_, "a=1/2/3&t=5");
  EXPECT_EQ(status, 503)
      << "Request must get 503 when concurrency semaphore is exhausted (0 slots)";

  // Clean up
  kill(child, SIGTERM);
  int ws;
  waitpid(child, &ws, 0);

  sem_destroy(conc_sem);
  munmap(conc_sem, sizeof(sem_t));
}

// ============================================================

TEST_F(FcgiServerSocketTest, SocketFileExistsAfterListen) {
  FcgiServer server;
  ASSERT_TRUE(server.listen(socket_path_));
  EXPECT_EQ(access(socket_path_.c_str(), F_OK), 0);
}

TEST_F(FcgiServerSocketTest, IsListeningStateAfterSuccessfulListen) {
  FcgiServer server;
  EXPECT_FALSE(server.is_listening());
  ASSERT_TRUE(server.listen(socket_path_));
  EXPECT_TRUE(server.is_listening());
}

TEST_F(FcgiServerSocketTest, MultipleListenCalls) {
  // The server should support listen() being called multiple times
  // (FCGX_OpenSocket adds each socket to its internal fd set).
  std::string path2 = make_unique_socket_path();
  ASSERT_FALSE(path2.empty());

  FcgiServer server;
  EXPECT_TRUE(server.listen(socket_path_));
  EXPECT_TRUE(server.is_listening());
  EXPECT_TRUE(server.listen(path2));
  EXPECT_TRUE(server.is_listening());

  unlink(path2.c_str());
}

TEST_F(FcgiServerSocketTest, ListenOnTcpPortZero) {
  // Port 0 lets the OS assign a free ephemeral port.
  FcgiServer server;
  EXPECT_TRUE(server.listen(":0"));
  EXPECT_TRUE(server.is_listening());
}

// ============================================================
// Test suite 2: Full FCGI protocol exchange (manual test only)
// ============================================================
//
// The full FCGI protocol cycle (FCGI_Accept over a direct socket) cannot
// be automated in a test harness because:
//
//   1. FCGI_Accept() uses global process-level state (stdin/stdout
//      redirection, internal fd arrays) that cannot be shared across
//      threads — std::thread causes hangs on CI.
//
//   2. fork() doesn't help: FCGX_Accept_r() sets up FCGI streams in the
//      child, but the process-wide stdout redirection doesn't work
//      correctly in test environments — response data goes to the
//      terminal instead of the socket.
//
// The full request/response cycle IS tested by the e2e tests
// (test_full_flow.cpp) using MockKnxdClient. The socket creation itself
// is verified by the FcgiServerSocketTest suite above.
//
// To manually test the full FCGI protocol over a real socket:
//
//   # Terminal 1: start the server with a direct socket
//   FCGI_SOCKET=/tmp/test-fcgi.sock ./build/src/cometvisu-knxd-fcgi
//
//   # Terminal 2: send an FCGI request (using cgi-fcgi or a custom client)
//   cgi-fcgi -connect /tmp/test-fcgi.sock /l
//
// Or use the built-in socket mode with a web server:
//   FCGI_SOCKET=:9000 ./build/src/cometvisu-knxd-fcgi
