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
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>

// Verify that SIGPIPE is ignored, so that write() on a closed socket
// returns EPIPE instead of killing the process.
//
// This is critical for the fork-based worker pool: when knxd closes a
// connection and a worker tries to write to it, the worker must survive
// to handle the error gracefully.

TEST(SigpipeTest, WriteToClosedSocketReturnsEpipe) {
  // First, ensure SIGPIPE is ignored (as main() does).
  // In a standalone test binary, the signal disposition is inherited
  // from the test runner.  We set it explicitly so the test is self-contained.
  ::signal(SIGPIPE, SIG_IGN);

  // Create a socket pair
  int sv[2];
  ASSERT_GE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  // Close one end — the other end is now a "broken pipe"
  ::close(sv[0]);

  // Try to write to the closed end.  With SIGPIPE ignored, this should
  // return -1 with errno=EPIPE.  Without SIG_IGN, the process would be
  // killed by SIGPIPE and never reach the assertion.
  errno = 0;
  const char data[] = "test";
  ssize_t ret = ::write(sv[1], data, sizeof(data));

  EXPECT_EQ(ret, -1);
  EXPECT_EQ(errno, EPIPE) << "Expected EPIPE, got: " << ::strerror(errno);

  ::close(sv[1]);
}

TEST(SigpipeTest, MultipleWritesToClosedSocketReturnEpipe) {
  ::signal(SIGPIPE, SIG_IGN);

  // Create a socket pair
  int sv[2];
  ASSERT_GE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  // Close one end — all subsequent writes to the other end should get EPIPE
  ::close(sv[0]);

  // First write — should get EPIPE
  errno = 0;
  const char data[] = "hello";
  ssize_t ret = ::write(sv[1], data, sizeof(data));
  EXPECT_EQ(ret, -1);
  EXPECT_EQ(errno, EPIPE) << "Expected EPIPE, got: " << ::strerror(errno);

  // Second write — should also get EPIPE (not SIGPIPE death)
  errno = 0;
  ret = ::write(sv[1], data, sizeof(data));
  EXPECT_EQ(ret, -1);
  EXPECT_EQ(errno, EPIPE) << "Expected EPIPE on second write, got: " << ::strerror(errno);

  // Third write — still EPIPE, process is still alive
  errno = 0;
  ret = ::write(sv[1], data, sizeof(data));
  EXPECT_EQ(ret, -1);
  EXPECT_EQ(errno, EPIPE) << "Expected EPIPE on third write, got: " << ::strerror(errno);

  ::close(sv[1]);
}
