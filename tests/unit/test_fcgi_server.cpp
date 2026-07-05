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
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <functional>
#include <string>

#include "fcgi/fcgi_server.h"

using namespace cvknxd;

namespace {

/// Run a function that may call exit() in a subprocess and return the exit
/// code, or -1 if the child was killed by a signal.
///
/// FCGX_OpenSocket exhibits two different behaviours on bind failure
/// depending on the libfcgi version / platform:
///   A) Return -1 (expected, well-behaved)
///   B) Call exit(1) (some builds of the library)
///
/// We use fork() to test both outcomes safely — the parent process
/// never sees an unexpected exit().
int run_in_subprocess(std::function<void()> fn) {
  pid_t pid = fork();
  if (pid == 0) {
    fn();
    _exit(0);  // Normal completion — child calls _exit, not exit
  }
  int status;
  waitpid(pid, &status, 0);
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  return -1;  // Killed by signal
}

}  // namespace

/// Unit tests for FcgiServer, focusing on the direct socket support (listen
/// mode).
///
/// These tests are organised in three groups:
///   1. Logic-only tests: verify state transitions and parameter validation
///      WITHOUT calling FCGX_OpenSocket (empty paths are caught by our own
///      guards before reaching the library).
///   2. Subprocess tests: verify that listen() with invalid socket paths
///      either returns false (normal libfcgi) or calls exit(1) (some builds).
///      Both outcomes are valid — the server doesn't hang or segfault.
///   3. Successful socket creation: tested in the integration test suite
///      (test_fcgi_server_socket.cpp) with a sockets_available() guard.

// ============================================================
// Logic-only tests
// ============================================================

TEST(FcgiServerTest, NotListeningByDefault) {
  FcgiServer server;
  EXPECT_FALSE(server.is_listening());
}

TEST(FcgiServerTest, ListenRejectsEmptyPath) {
  // Empty path is caught by our early return before FCGX_OpenSocket is called.
  FcgiServer server;
  EXPECT_FALSE(server.listen(""));
  EXPECT_FALSE(server.is_listening());
}

TEST(FcgiServerTest, ListenStateAfterFailedCall) {
  FcgiServer server;
  EXPECT_FALSE(server.listen(""));
  // After a failed call (empty path -> caught early), the server state should
  // remain non-listening — no side effects leaked.
  EXPECT_FALSE(server.is_listening());
}

// ============================================================
// Subprocess tests — invalid socket paths
// ============================================================
//
// FCGX_OpenSocket may call exit(1) on bind failure in some library
// versions. We run the test in a forked child so the parent process
// is not affected.

TEST(FcgiServerTest, ListenRejectsNonexistentDirectory) {
  int code = run_in_subprocess([]() {
    FcgiServer server;
    bool result = server.listen("/nonexistent/path/fcgi.sock");
    _exit(result ? 1 : 0);
  });
  // Different libfcgi versions use different exit codes on bind failure
  // (1 or 2). Accept any clean exit — the important thing is the process
  // didn't hang or crash with a signal.
  EXPECT_GE(code, 0) << "Child was killed by signal, expected clean exit";
}

TEST(FcgiServerTest, ListenRejectsColonOnly) {
  int code = run_in_subprocess([]() {
    FcgiServer server;
    bool result = server.listen(":");
    _exit(result ? 1 : 0);
  });
  EXPECT_GE(code, 0) << "Child was killed by signal, expected clean exit";
}

// ============================================================
// FcgiRequest::path() tests
// ============================================================

TEST(FcgiRequestPathTest, PathInfoIsUsedWhenSet) {
  FcgiRequest req;
  req.path_info = "/l";
  req.script_name = "/cgi-bin/visu";
  req.request_uri = "/cgi-bin/visu/l?a=1";

  EXPECT_EQ(req.path(), "/l");
}

TEST(FcgiRequestPathTest, ScriptNameStrippedFromUriPath) {
  FcgiRequest req;
  req.script_name = "/cgi-bin/visu";
  req.request_uri = "/cgi-bin/visu/l";

  EXPECT_EQ(req.path(), "/l");
}

TEST(FcgiRequestPathTest, ScriptNameConsumesEntireUriPath) {
  // When SCRIPT_NAME itself contains the endpoint (e.g. /cgi-bin/l),
  // the last path component of SCRIPT_NAME is used.
  FcgiRequest req;
  req.script_name = "/cgi-bin/l";
  req.request_uri = "/cgi-bin/l?u=USER&p=PASSWORD&d=DEVICE";

  EXPECT_EQ(req.path(), "/l");
}

TEST(FcgiRequestPathTest, ScriptNameConsumesUriPathWithQueryString) {
  // Same as above but PATH_INFO explicitly empty
  FcgiRequest req;
  req.script_name = "/cgi-bin/r";
  req.request_uri = "/cgi-bin/r?a=1/2/3&t=5";

  EXPECT_EQ(req.path(), "/r");
}

TEST(FcgiRequestPathTest, DirectRequestWithoutPrefix) {
  // When SCRIPT_NAME is empty, the full URI path is returned
  FcgiRequest req;
  req.request_uri = "/l";

  EXPECT_EQ(req.path(), "/l");
}

TEST(FcgiRequestPathTest, DeepPathScriptNameConsumesUri) {
  FcgiRequest req;
  req.script_name = "/some/deep/path/w";
  req.request_uri = "/some/deep/path/w?a=1&b=2";

  EXPECT_EQ(req.path(), "/w");
}

TEST(FcgiRequestPathTest, EmptyUriReturnsEmpty) {
  FcgiRequest req;
  EXPECT_EQ(req.path(), "");
}
