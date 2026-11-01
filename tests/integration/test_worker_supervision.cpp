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
 * @file test_worker_supervision.cpp
 * @brief The parent must replace dead children (workers and cache reader).
 *
 * A dead worker silently shrinks the pool, and a dead cache reader means no
 * telegram ever reaches the shared cache again so /r keeps serving stale
 * values forever.  Both used to go unnoticed because the SIGCHLD handler
 * reaped the children before the supervisor loop could see the exit status.
 *
 * This test runs the real binary (path passed in at compile time) and kills
 * children with SIGKILL.
 */

#include <dirent.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

#ifndef CVKNXD_BINARY
#error "CVKNXD_BINARY must be defined to the cometvisu-knxd-fcgi executable"
#endif

/// Child PIDs whose parent is @p parent (read from /proc/<pid>/stat).
std::vector<pid_t> list_children(pid_t parent) {
  std::vector<pid_t> children;
  DIR* proc = ::opendir("/proc");
  if (proc == nullptr) {
    return children;
  }
  while (const dirent* entry = ::readdir(proc)) {
    if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
      continue;
    }
    std::ifstream stat_file(std::string("/proc/") + entry->d_name + "/stat");
    std::string line;
    if (!std::getline(stat_file, line)) {
      continue;
    }
    // Format: "pid (comm) state ppid ..." — comm may contain spaces/parens.
    const auto close_paren = line.rfind(')');
    if (close_paren == std::string::npos || close_paren + 2 >= line.size()) {
      continue;
    }
    std::istringstream rest(line.substr(close_paren + 2));
    char state = 0;
    long long ppid = 0;
    rest >> state >> ppid;
    if (ppid == parent) {
      children.push_back(static_cast<pid_t>(std::atoi(entry->d_name)));
    }
  }
  ::closedir(proc);
  std::sort(children.begin(), children.end());
  return children;
}

bool wait_for(const std::function<bool()>& predicate, int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return predicate();
}

bool contains(const std::vector<pid_t>& pids, pid_t pid) {
  return std::find(pids.begin(), pids.end(), pid) != pids.end();
}

class WorkerSupervisionTest : public ::testing::Test {
protected:
  void SetUp() override {
    if (::access("/proc/self/stat", R_OK) != 0) {
      GTEST_SKIP() << "/proc is not available";
    }
    char dir_template[] = "/tmp/cvknxd-supervision-XXXXXX";
    const char* dir = ::mkdtemp(dir_template);
    ASSERT_NE(dir, nullptr) << "mkdtemp failed";
    dir_ = dir;
    socket_path_ = dir_ + "/fcgi.sock";
    knxd_path_ = dir_ + "/no-such-knxd";  // intentionally absent

    server_pid_ = ::fork();
    ASSERT_GE(server_pid_, 0) << "fork failed";
    if (server_pid_ == 0) {
      // Silence the backend's own logging so it does not interleave with gtest.
      const int devnull = ::open("/dev/null", O_WRONLY);
      if (devnull >= 0) {
        ::dup2(devnull, STDOUT_FILENO);
        ::dup2(devnull, STDERR_FILENO);
        ::close(devnull);
      }
      ::setenv("FCGI_SOCKET", socket_path_.c_str(), 1);
      ::setenv("KNXD_SOCKET", knxd_path_.c_str(), 1);
      ::setenv("FCGI_WORKERS", "2", 1);
      ::execl(CVKNXD_BINARY, CVKNXD_BINARY, nullptr);
      ::_exit(127);
    }

    ASSERT_TRUE(wait_for([this] { return ::access(socket_path_.c_str(), F_OK) == 0; }, 10000))
        << "backend never opened its FCGI socket";
    ASSERT_TRUE(wait_for([this] { return list_children(server_pid_).size() >= 3; }, 10000))
        << "backend never started its children (cache reader + 2 workers)";
  }

  void TearDown() override {
    if (server_pid_ > 0) {
      ::kill(server_pid_, SIGTERM);
      int status = 0;
      ::waitpid(server_pid_, &status, 0);
      server_pid_ = -1;
    }
    if (!dir_.empty()) {
      ::unlink(socket_path_.c_str());
      ::rmdir(dir_.c_str());
    }
  }

  /// Kill @p victim with SIGKILL and wait until exactly @p expected_children
  /// children exist again and the victim is gone.
  void expect_replaced(pid_t victim, size_t expected_children) {
    ASSERT_EQ(::kill(victim, SIGKILL), 0);
    EXPECT_TRUE(wait_for(
        [this, victim, expected_children] {
          const auto children = list_children(server_pid_);
          return children.size() == expected_children && !contains(children, victim);
        },
        5000))
        << "pid " << victim << " was not replaced (pool size did not recover)";
  }

  std::string dir_;
  std::string socket_path_;
  std::string knxd_path_;
  pid_t server_pid_ = -1;
};

TEST_F(WorkerSupervisionTest, RestartsAWorkerThatDies) {
  const auto children = list_children(server_pid_);
  ASSERT_EQ(children.size(), 3U);  // cache reader + 2 workers

  // The workers are forked after the cache reader, so they have higher pids.
  const pid_t worker = children.back();
  expect_replaced(worker, 3);
}

TEST_F(WorkerSupervisionTest, RestartsTheCacheReader) {
  const auto children = list_children(server_pid_);
  ASSERT_EQ(children.size(), 3U);

  // The cache reader is forked first (lowest pid).
  const pid_t cache_reader = children.front();
  expect_replaced(cache_reader, 3);
}

TEST_F(WorkerSupervisionTest, TerminatesAllChildrenOnSigterm) {
  ::kill(server_pid_, SIGTERM);
  EXPECT_TRUE(wait_for([this] { return list_children(server_pid_).empty(); }, 5000))
      << "children survived shutdown";

  int status = 0;
  ASSERT_EQ(::waitpid(server_pid_, &status, 0), server_pid_);
  server_pid_ = -1;  // reaped — do not kill it again in TearDown
  EXPECT_TRUE(WIFEXITED(status));
}

}  // namespace
