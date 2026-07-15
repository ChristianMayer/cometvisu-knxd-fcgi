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

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "fcgi/fcgi_server.h"
#include "knxd/knxd_client.h"
#include "knxd/knxd_connector.h"
#include "knxd/knxd_protocol.h"
#include "router/router.h"
#include "state/session_store.h"
#include "util/debug_log.h"
#include "version.h"

namespace {

/// Maximum long-poll timeout in seconds. Prevents DoS via huge timeout values.
inline constexpr int kMaxLongpollTimeoutSec = 3600;  // 1 hour

/// Read an environment variable with a default value.
const char* get_env_default(const char* name, const char* default_value) {
  const char* val = getenv(name);
  return (val != nullptr && val[0] != '\0') ? val : default_value;
}

/// Parse an integer from an environment variable safely using std::from_chars.
/// Returns the parsed value clamped to [min_val, max_val], or default_val on error.
int parse_env_int(const char* name, int default_val, int min_val, int max_val) {
  const char* val = getenv(name);
  if (val == nullptr || val[0] == '\0')
    return default_val;
  int result = 0;
  auto [ptr, ec] = std::from_chars(val, val + std::strlen(val), result);
  if (ec != std::errc{} || result < min_val)
    return default_val;
  if (result > max_val)
    return max_val;
  return result;
}

}  // namespace

int main(int argc, char* argv[]) {
  using namespace cvknxd;

  // Handle command-line flags when invoked directly (e.g., for version queries).
  // FastCGI binaries can be run from a terminal for introspection without
  // needing a running web server.
  if (argc > 1) {
    std::string_view arg = argv[1];
    if (arg == "--version" || arg == "-v") {
      std::cout << application_name() << " " << version() << "\n";
      return 0;
    }
    if (arg == "--help" || arg == "-h") {
      std::cout
          << application_name() << " " << version() << "\n"
          << "FastCGI backend bridging CometVisu Protocol to knxd\n"
          << "\n"
          << "Usage: cometvisu-knxd-fcgi [OPTION]\n"
          << "\n"
          << "Options:\n"
          << "  --version, -v   Print version information and exit\n"
          << "  --help, -h      Print this help message and exit\n"
          << "\n"
          << "Environment variables:\n"
          << "  KNXD_SOCKET           Path to the knxd Unix socket (default: /run/knx)\n"
          << "  FCGI_SOCKET           Direct FCGI socket (unset = spawn-fcgi mode)\n"
          << "  FCGI_WORKERS          Worker processes in direct socket mode (default: 20, "
             "max: 256)\n"
          << "  LONGPOLL_TIMEOUT_SEC  Max seconds to wait in long-poll /r (default: 300)\n"
          << "  ADDRESS_PREFIX        Namespace prefix for addresses without explicit prefix\n"
          << "                        (default: empty = no prefix, e.g. set to \"KNX\")\n"
          << "  BASE_URL              URL prefix advertised in login response (unset = omitted)\n"
          << "                        (default: not set, e.g. /proxy/visu)\n"
          << "  DEBUG_BACKEND         Set to 1 to enable debug logging to stderr\n"
          << "\n"
          << "When run without options, starts the FastCGI server loop.\n";
      return 0;
    }
  }

  // ---- Debug mode ----
  // Enable via environment variable DEBUG_BACKEND=1 (or DEBUG_BACKEND=true/yes/on).
  // When enabled, all HTTP request/response cycles and knxd communication
  // are printed to stderr for troubleshooting.
  DebugLog::init_from_env();

  // ---- Configuration ----
  // Environment variables are inherited from the FCGI-spawning web server.
  // This is safe because:
  //   - The web server (e.g. Apache, nginx, lighttpd) is the trusted parent process.
  //   - The knxd socket is a local Unix socket — redirection only affects the local machine.
  //   - An attacker who can manipulate the web server's environment already has
  //     sufficient access to compromise the system directly.
  const char* knxd_socket = get_env_default("KNXD_SOCKET", "/run/knx");
  int longpoll_timeout = parse_env_int("LONGPOLL_TIMEOUT_SEC", 300, 1, kMaxLongpollTimeoutSec);
  int num_workers = parse_env_int("FCGI_WORKERS", 20, 1, 256);

  // ---- Address prefix (namespace) ----
  // When the client sends addresses without a namespace prefix (no colon),
  // this value is used as the default namespace. Default: empty (no prefix).
  // Set ADDRESS_PREFIX=KNX to restore the old "KNX:1/2/3" format.
  const char* address_prefix = get_env_default("ADDRESS_PREFIX", "");
  KnxAddress::set_default_namespace(address_prefix);

  // ---- Base URL for login response ----
  // When set, the /l endpoint includes a "c" object with "baseURL" set to this value.
  const char* base_url = getenv("BASE_URL");

  // ---- Optional direct socket (standalone mode) ----
  // Set FCGI_SOCKET to a TCP port (":9000") or Unix socket path to run without
  // spawn-fcgi. Read early so startup info can show it.
  const char* fcgi_socket = get_env_default("FCGI_SOCKET", "");

  // ---- Startup info ----
  std::cout << "[INFO] cometvisu-knxd-fcgi " << version() << " starting\n";
  std::cout << "[INFO] Configuration:\n";
  std::cout << "[INFO]   KNXD_SOCKET           " << knxd_socket << "\n";
  std::cout << "[INFO]   FCGI_SOCKET           "
            << (fcgi_socket[0] != '\0' ? fcgi_socket : "(not set, spawn-fcgi mode)") << "\n";
  std::cout << "[INFO]   FCGI_WORKERS          " << num_workers << "\n";
  std::cout << "[INFO]   LONGPOLL_TIMEOUT_SEC  " << longpoll_timeout << "\n";
  std::cout << "[INFO]   ADDRESS_PREFIX        "
            << (address_prefix[0] != '\0' ? address_prefix : "(not set)") << "\n";
  std::cout << "[INFO]   BASE_URL              "
            << (base_url != nullptr && base_url[0] != '\0' ? base_url : "(not set)") << "\n";
  std::cout << "[INFO]   DEBUG_BACKEND         "
            << (getenv("DEBUG_BACKEND") != nullptr ? getenv("DEBUG_BACKEND") : "(not set)") << "\n";

  // ---- Initialize FCGI server (before knxd) ----
  // Set up the FastCGI listen socket first so it's ready for connections
  // before attempting the potentially slow knxd connection.
  // In spawn-fcgi mode (no FCGI_SOCKET), the web server handles stdin/stdout.
  FcgiServer server;
  if (fcgi_socket[0] != '\0') {
    if (server.listen(fcgi_socket)) {
      std::cout << "[INFO] Direct FCGI socket: " << fcgi_socket << "\n";
    } else {
      std::cerr << "[ERROR] Failed to open FCGI socket: " << fcgi_socket << "\n";
      return 1;
    }
  }

  // ---- Connect to knxd with retry ----
  // First-time connection (has_worked_before=false): retry after 500ms, 1s, 2s.
  // If the connection was previously successful (reconnect), extended retries
  // at 4s and 8s are also attempted.
  KnxdClient knxd;
  if (!connect_knxd_with_retry(knxd, knxd_socket, false)) {
    std::cerr << "[ERROR] Cannot connect to knxd at " << knxd_socket << " after all retries\n";
    return 1;
  }

  // Open group socket for sending and receiving
  if (!knxd.open_group_socket(false)) {
    std::cerr << "[ERROR] Cannot open group socket on knxd\n";
    knxd.disconnect();
    return 1;
  }

  // Set non-blocking mode for efficient poll()-based long-poll
  knxd.set_nonblocking(true);

  SessionStore sessions;

  // ---- Create router ----
  // No local cache — we delegate to knxd's built-in cache via cache_read().
  Router router(knxd, sessions, longpoll_timeout);

  // Register the request handler on the FCGI server
  server.set_handler([&](const FcgiRequest& req) -> FcgiResponse { return router.route(req); });

  // ---- Run ----
  int result = 0;
  if (fcgi_socket[0] != '\0') {
    // Fork-based worker pool: each child process runs an independent
    // accept loop on the shared listen socket. The OS serializes accept()
    // across processes.
    //
    // This avoids Docker < 20.10.10 seccomp issues where clone3/clone
    // with process-sharing flags (CLONE_VM, CLONE_THREAD, etc.) are
    // blocked, preventing std::thread creation.  fork() uses plain process cloning
    // which is permitted by all seccomp profiles.
    //
    // Each child opens its own knxd connection to avoid contention on
    // the shared file descriptor.  Session state is per-process (same
    // limitation as spawn-fcgi mode).
    std::cout << "[INFO] Starting " << num_workers << " worker processes\n";

    pid_t parent_pid = ::getpid();
    (void)parent_pid;  // used for debugging / signal filtering if needed
    std::vector<pid_t> worker_pids;
    worker_pids.reserve(static_cast<size_t>(num_workers));

    bool fork_failed = false;
    int fork_errno = 0;

    for (int i = 0; i < num_workers; ++i) {
      pid_t pid = ::fork();
      if (pid < 0) {
        fork_errno = errno;
        std::cerr << "[ERROR] fork() for worker " << (i + 1) << "/" << num_workers
                  << " failed: " << std::strerror(fork_errno) << " (errno=" << fork_errno << ")\n";
        fork_failed = true;
        break;
      }

      if (pid == 0) {
        // ================================================
        // Child process — worker
        // ================================================

        // Close the inherited knxd connection.  Each worker
        // opens its own to avoid contention on the shared fd.
        knxd.disconnect();

        // The parent successfully connected to knxd, so use extended retry delays
        // (500ms, 1s, 2s, 4s, 8s) in case knxd is temporarily unavailable.
        if (!connect_knxd_with_retry(knxd, knxd_socket, true)) {
          std::cerr << "[ERROR] Worker pid=" << ::getpid() << " (worker " << i
                    << "): Cannot connect to knxd at " << knxd_socket << " after all retries\n";
          std::_Exit(1);
        }

        if (!knxd.open_group_socket(false)) {
          std::cerr << "[ERROR] Worker pid=" << ::getpid() << " (worker " << i
                    << "): Cannot open group socket "
                    << "on knxd at " << knxd_socket << " (check knxd is running and accessible)\n";
          std::_Exit(2);
        }

        knxd.set_nonblocking(true);
        std::cout << "[INFO] Worker " << i << " ready, pid=" << ::getpid() << "\n";

        // Run the accept loop.  Blocks until the listen socket is
        // shut down (by parent SIGTERM).
        int child_result = server.run();
        knxd.disconnect();
        std::_Exit(child_result);
      }

      // Parent continues: record the child PID
      worker_pids.push_back(pid);
      std::cout << "[INFO] Started worker " << i << "/" << num_workers << " pid=" << pid << "\n";
    }

    if (fork_failed) {
      // Kill all successfully started children
      std::cerr << "[ERROR] fork() failed for worker " << (worker_pids.size() + 1) << "/"
                << num_workers << ", terminating " << worker_pids.size()
                << " already-started worker(s)\n";
      for (pid_t pid : worker_pids) {
        ::kill(pid, SIGTERM);
      }
      // Wait for killed children
      int status;
      for (size_t k = 0; k < worker_pids.size(); ++k) {
        ::wait(&status);
      }
      result = 1;
    } else {
      std::cout << "[INFO] All " << num_workers << " workers started, waiting for children...\n";

      // Wait for all child processes.  When a child exits
      // unexpectedly, log it and continue waiting for others.
      int status;
      pid_t waited;
      while ((waited = ::wait(&status)) > 0) {
        if (WIFEXITED(status)) {
          int exit_code = WEXITSTATUS(status);
          if (exit_code != 0) {
            std::cerr << "[WARN] Worker pid=" << waited << " exited with code " << exit_code
                      << "\n";
          }
        } else if (WIFSIGNALED(status)) {
          std::cerr << "[WARN] Worker pid=" << waited << " killed by signal " << WTERMSIG(status);
          if (WCOREDUMP(status)) {
            std::cerr << " (core dumped)";
          }
          std::cerr << "\n";
        }
      }
      result = 0;
    }
  } else {
    // Spawn-fcgi mode: concurrency is handled by running multiple
    // process instances via spawn-fcgi (e.g., spawn-fcgi -F N).
    result = server.run();
  }

  // Cleanup
  knxd.disconnect();

  return result;
}
