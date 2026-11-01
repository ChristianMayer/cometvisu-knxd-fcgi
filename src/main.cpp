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
 * @file main.cpp
 * @brief Application entry point — configuration, signal handling, worker
 *        pool, and the main event loop.
 *
 * This is the equivalent of running eibread-cgi and eibwrite-cgi as a
 * single unified binary, with these key differences:
 *   - Fork-based worker pool instead of spawn-fcgi's multi-instance model
 *   - Persistent knxd connections instead of per-request connect/disconnect
 *   - Direct socket mode (FCGI_SOCKET) for standalone operation
 *   - Load shedding semaphores to prevent long-poll head-of-line blocking
 *   - Signal handling for graceful shutdown (SIGTERM, SIGCHLD)
 *   - Resource limits (RLIMIT_NOFILE, RLIMIT_AS) for embedded safety
 *
 * Startup order matters: the FCGI listen socket is opened BEFORE the knxd
 * connection, so the web server can connect even while we're retrying knxd.
 *
 * @see AGENTS.md for the full design rationale.
 */

#include <semaphore.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "fcgi/fcgi_server.h"
#include "knxd/cache_reader.h"
#include "knxd/knxd_client.h"
#include "knxd/knxd_connector.h"
#include "knxd/knxd_protocol.h"
#include "router/router.h"
#include "state/session_store.h"
#include "state/shared_group_cache.h"
#include "util/debug_log.h"
#include "version.h"

namespace {

/// Maximum long-poll timeout in seconds. Prevents DoS via huge timeout values.
inline constexpr int kMaxLongpollTimeoutSec = 3600;  // 1 hour

/// Maximum file descriptors per process. Prevents fd exhaustion on
/// long-running embedded systems where slow leaks accumulate over years.
inline constexpr rlim_t kMaxFileDescriptors = 256;

/// Maximum virtual memory per worker process (64 MB). Prevents a single
/// worker from exhausting system memory via a leak or unbounded growth.
inline constexpr rlim_t kMaxWorkerMemoryBytes = 64ULL * 1024 * 1024;

// ---- Signal handling ----
// Signal handlers run in a restricted context (only async-signal-safe
// functions are permitted).  They set volatile flags which the main loop
// checks at well-defined points.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
volatile sig_atomic_t g_shutdown_requested = 0;

extern "C" void handle_sigterm(int /*signum*/) {
  g_shutdown_requested = 1;
}

/// Set resource limits for long-running embedded operation.
/// Called once at startup (before fork) so all children inherit the limits.
void set_resource_limits() {
  struct rlimit rl{};

  // Limit file descriptors to prevent exhaustion from slow leaks.
  rl.rlim_cur = kMaxFileDescriptors;
  rl.rlim_max = kMaxFileDescriptors;
  if (::setrlimit(RLIMIT_NOFILE, &rl) != 0) {
    std::cerr << "[WARN] Failed to set RLIMIT_NOFILE: " << std::strerror(errno) << "\n";
  }

  // Limit virtual memory per worker. Only the soft limit is set so
  // the process can still be profiled/debugged with a higher hard limit.
  // Children inherit this and optionally tighten it further.
  rl.rlim_cur = kMaxWorkerMemoryBytes;
  rl.rlim_max = RLIM_INFINITY;
  if (::setrlimit(RLIMIT_AS, &rl) != 0) {
    std::cerr << "[WARN] Failed to set RLIMIT_AS: " << std::strerror(errno) << "\n";
  }
  // Note: RLIMIT_AS may be reset per-worker in the child if tighter
  // limits are needed. Currently all workers share the same limit.
}

/// Read an environment variable with a default value.
const char* get_env_default(const char* name, const char* default_value) {
  const char* val = getenv(name);
  return (val != nullptr && *val != '\0') ? val : default_value;
}

/// Parse an integer from an environment variable safely using std::strtol.
/// Returns the parsed value clamped to [min_val, max_val], or default_val on error.
int parse_env_int(const char* name, int default_val, int min_val, int max_val) {
  const char* val = getenv(name);
  if (val == nullptr || *val == '\0') {
    return default_val;
  }
  char* end = nullptr;
  int64_t raw = std::strtol(val, &end, 10);
  if (end == val || *end != '\0' || raw < static_cast<int64_t>(min_val)) {
    return default_val;
  }
  int result = static_cast<int>(raw);
  if (result > max_val) {
    return max_val;
  }
  return result;
}

}  // namespace

int main(int argc, char* argv[]) {
  using namespace cvknxd;

  // Handle command-line flags when invoked directly (e.g., for version queries).
  // FastCGI binaries can be run from a terminal for introspection without
  // needing a running web server.
  if (argc > 1) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const std::string_view arg = argv[1];
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
          << "  LOGIN_EXTRA_CONFIG    Raw JSON key-value pairs injected into login response\n"
          << "                        \"c\" block (empty = none)\n"
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

  // ---- Signal handling ----
  // Ignore SIGPIPE so that write() on a closed socket returns EPIPE instead
  // of killing the process. This is essential in the fork-based worker pool
  // where knxd may close connections asynchronously. The write_all() error
  // handling already copes with EPIPE correctly — it just needs the process
  // to stay alive to handle it.
  (void)::signal(SIGPIPE, SIG_IGN);

  // Handle SIGTERM for graceful shutdown. The parent forwards the signal
  // to all children, waits for them to exit, then exits itself.
  //
  // sigaction() with sa_flags = 0 (i.e. WITHOUT SA_RESTART) is required: with
  // the default BSD semantics of signal() the supervisor's wait() is restarted
  // transparently after the handler returns, so the shutdown flag is never
  // seen and the parent keeps waiting forever.
  struct sigaction shutdown_action{};
  shutdown_action.sa_handler = handle_sigterm;
  ::sigemptyset(&shutdown_action.sa_mask);
  shutdown_action.sa_flags = 0;
  if (::sigaction(SIGTERM, &shutdown_action, nullptr) != 0) {
    std::cerr << "[WARN] Failed to install SIGTERM handler: " << std::strerror(errno) << "\n";
  }

  // NOTE: no SIGCHLD handler.  The parent's supervisor loop calls wait() and
  // must see every exit status — a handler that calls waitpid() would reap the
  // children first, so dead workers would never be restarted.

  // ---- Resource limits for embedded long-running operation ----
  set_resource_limits();

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
            << (*fcgi_socket != '\0' ? fcgi_socket : "(not set, spawn-fcgi mode)") << "\n";
  std::cout << "[INFO]   FCGI_WORKERS          " << num_workers << "\n";
  std::cout << "[INFO]   LONGPOLL_TIMEOUT_SEC  " << longpoll_timeout << "\n";
  std::cout << "[INFO]   ADDRESS_PREFIX        "
            << (*address_prefix != '\0' ? address_prefix : "(not set)") << "\n";
  std::cout << "[INFO]   BASE_URL              "
            << (base_url != nullptr && *base_url != '\0' ? base_url : "(not set)") << "\n";
  std::cout << "[INFO]   LOGIN_EXTRA_CONFIG    "
            << (getenv("LOGIN_EXTRA_CONFIG") != nullptr ? getenv("LOGIN_EXTRA_CONFIG")
                                                        : "(not set)")
            << "\n";
  std::cout << "[INFO]   DEBUG_BACKEND         "
            << (getenv("DEBUG_BACKEND") != nullptr ? getenv("DEBUG_BACKEND") : "(not set)") << "\n";

  // ---- Initialize FCGI server (before knxd) ----
  // Set up the FastCGI listen socket first so it's ready for connections
  // before attempting the potentially slow knxd connection.
  // In spawn-fcgi mode (no FCGI_SOCKET), the web server handles stdin/stdout.
  FcgiServer server;
  if (*fcgi_socket != '\0') {
    if (server.listen(fcgi_socket)) {
      std::cout << "[INFO] Direct FCGI socket: " << fcgi_socket << "\n";
    } else {
      std::cerr << "[ERROR] Failed to open FCGI socket: " << fcgi_socket << "\n";
      return 1;
    }
  }

  // ---- Create shared group cache (in mmap'd shared memory) ----
  // All worker processes access this single cache.  A dedicated cache reader
  // process drains the single knxd group socket and pushes telegrams here.
  // The process-shared mutex and condition variable provide synchronization
  // and zero-CPU wakeup for long-poll readers.
  SharedGroupCache shared_cache;
  if (!shared_cache.create()) {
    std::cerr << "[ERROR] Failed to create shared group cache\n";
    return 1;
  }
  std::cout << "[INFO] Shared group cache created (" << SharedGroupCache::region_size()
            << " bytes, capacity=" << kSharedCacheCapacity << ")\n";

  // ---- Verify the knxd connection (best effort) ----
  // The daemon MUST start even when knxd is unavailable: knxd can die at any
  // time ("Link down, terminating") and is often restarted by a supervisor.
  // The cache reader and the workers reconnect on demand, so a failed check is
  // only a warning — never a reason to refuse to serve HTTP.
  KnxdClient knxd_parent;
  if (connect_knxd_with_retry(knxd_parent, knxd_socket, false)) {
    if (knxd_parent.open_group_socket(false)) {
      knxd_parent.set_nonblocking(true);
      std::cout << "[INFO] knxd connection verified at " << knxd_socket << "\n";
    } else {
      std::cerr << "[WARN] knxd at " << knxd_socket
                << " does not accept a group socket — continuing, retrying in the background\n";
    }
    knxd_parent.disconnect();
  } else {
    std::cerr << "[WARN] knxd not reachable at " << knxd_socket
              << " — starting anyway and retrying in the background\n";
  }

  SessionStore sessions;

  // ---- Create router (will be used by worker processes) ----
  // The knxd reference is for writes only — workers reconnect it as write-only.
  KnxdClient knxd_writer;
  Router router(knxd_writer, shared_cache, sessions, longpoll_timeout,
                base_url != nullptr ? base_url : "");

  // Register the request handler on the FCGI server
  server.set_handler([&](const FcgiRequest& req) -> FcgiResponse { return router.route(req); });

  // ---- Run ----
  int result = 0;
  if (*fcgi_socket != '\0') {
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
    std::cout << std::flush;  // Flush before fork to avoid buffer duplication in children

    // ---- Load shedding semaphore ----
    // Shared semaphore to limit concurrent long-poll reads across workers.
    // Initialized to num_workers so that when all workers are busy with
    // long-poll reads, new read requests return empty immediately instead
    // of queuing in the listen backlog.  Writes and logins never take a
    // token, so they are always served.
    sem_t* load_shed_sem = static_cast<sem_t*>(
        ::mmap(nullptr, sizeof(sem_t), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    if (load_shed_sem == MAP_FAILED) {
      std::cerr << "[WARN] Failed to create load-shed semaphore: " << std::strerror(errno)
                << " (continuing without load shedding)\n";
      load_shed_sem = nullptr;
    } else if (::sem_init(load_shed_sem, 1, static_cast<unsigned>(num_workers)) != 0) {
      std::cerr << "[WARN] Failed to init load-shed semaphore: " << std::strerror(errno)
                << " (continuing without load shedding)\n";
      ::munmap(load_shed_sem, sizeof(sem_t));
      load_shed_sem = nullptr;
    }
    server.set_load_shed_semaphore(load_shed_sem);

    // ---- Concurrency limiting semaphore ----
    // Caps how many long-poll reads may run at the same time.  When
    // exhausted, /r returns HTTP 503 immediately instead of piling up in the
    // listen backlog (which makes the reverse proxy report 502).  Writes and
    // logins never take a token — a spurious 503 would surface in CometVisu
    // as a failed switch operation.
    sem_t* concurrency_sem = static_cast<sem_t*>(
        ::mmap(nullptr, sizeof(sem_t), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    if (concurrency_sem == MAP_FAILED) {
      std::cerr << "[WARN] Failed to create concurrency semaphore: " << std::strerror(errno)
                << " (continuing without concurrency limiting)\n";
      concurrency_sem = nullptr;
    } else if (::sem_init(concurrency_sem, 1, static_cast<unsigned>(num_workers)) != 0) {
      std::cerr << "[WARN] Failed to init concurrency semaphore: " << std::strerror(errno)
                << " (continuing without concurrency limiting)\n";
      ::munmap(concurrency_sem, sizeof(sem_t));
      concurrency_sem = nullptr;
    }
    server.set_concurrency_semaphore(concurrency_sem);

    pid_t parent_pid = ::getpid();
    (void)parent_pid;  // used for debugging / signal filtering if needed

    // ---- Child factories ----
    // Both factories fork; the child runs the given body and never returns
    // (it exits through _Exit()), the parent gets the pid (or -1 on failure).
    //
    // A dedicated cache reader drains the single knxd group socket and pushes
    // telegrams into the shared cache.  Workers only open write-only knxd
    // connections — all group telegram reception goes through the reader.
    const std::function<pid_t()> spawn_cache_reader = [&]() -> pid_t {
      const pid_t pid = ::fork();
      if (pid < 0) {
        return -1;
      }
      if (pid == 0) {
        // ================================================
        // Cache reader child process
        // ================================================
        ::prctl(PR_SET_PDEATHSIG, SIGTERM);
        (void)::signal(SIGTERM, SIG_DFL);

        // Close inherited FCGI resources — cache reader doesn't handle HTTP.
        if (server.is_listening()) {
          server.close_listen_socket();
        }

        // The reader owns its own knxd connection and reconnects forever:
        // knxd may be missing at startup or restart at any time, and the loop
        // must wait for it without burning CPU.  SIGTERM (PR_SET_PDEATHSIG
        // above) terminates the reader; the stop predicate keeps the loop
        // stoppable and testable.
        KnxdClient reader_knxd;
        std::cout << "[INFO] Cache reader started, pid=" << ::getpid() << "\n";
        std::cout << std::flush;

        CacheReaderOptions reader_options;
        reader_options.socket_path = knxd_socket;

        run_cache_reader(reader_knxd, shared_cache, reader_options,
                         [] { return g_shutdown_requested != 0; });

        std::cout << "[INFO] Cache reader shutting down\n";
        std::cout << std::flush;
        reader_knxd.disconnect();
        std::_Exit(0);
      }
      return pid;
    };

    const std::function<pid_t(int)> spawn_worker = [&](int index) -> pid_t {
      const pid_t pid = ::fork();
      if (pid < 0) {
        return -1;
      }
      if (pid == 0) {
        // ================================================
        // Worker child process
        // ================================================
        ::prctl(PR_SET_PDEATHSIG, SIGTERM);
        (void)::signal(SIGTERM, SIG_DFL);

        // Workers use write-only group sockets — the cache reader handles all
        // group telegram reception.
        //
        // A missing knxd must not keep the worker from serving HTTP and must
        // not delay its readiness: try once, warn on failure, and let
        // send_group_packet() reconnect on demand (connect() remembers the
        // socket path even when the attempt failed).
        if (!knxd_writer.connect(knxd_socket)) {
          std::cerr << "[WARN] Worker pid=" << ::getpid() << " (worker " << index
                    << "): knxd not reachable at " << knxd_socket
                    << " — continuing, will retry on demand\n";
        } else if (!knxd_writer.open_group_socket(true)) {
          std::cerr << "[WARN] Worker pid=" << ::getpid() << " (worker " << index
                    << "): cannot open group socket on knxd at " << knxd_socket
                    << " — continuing, will retry on demand\n";
          knxd_writer.disconnect();
        }

        std::cout << "[INFO] Worker " << index << " ready, pid=" << ::getpid() << "\n";
        std::cout << std::flush;

        const int child_result = server.run();
        knxd_writer.disconnect();
        std::_Exit(child_result);
      }
      return pid;
    };

    // ---- Start the cache reader and the worker pool ----
    pid_t cache_reader_pid = spawn_cache_reader();
    if (cache_reader_pid < 0) {
      std::cerr << "[ERROR] fork() for cache reader failed: " << std::strerror(errno) << "\n";
      return 1;
    }
    std::cout << "[INFO] Started cache reader pid=" << cache_reader_pid << "\n";

    std::vector<pid_t> worker_pids(static_cast<size_t>(num_workers), -1);
    bool fork_failed = false;
    for (int i = 0; i < num_workers; ++i) {
      const pid_t pid = spawn_worker(i);
      if (pid < 0) {
        std::cerr << "[ERROR] fork() for worker " << (i + 1) << "/" << num_workers
                  << " failed: " << std::strerror(errno) << " (errno=" << errno << ")\n";
        fork_failed = true;
        break;
      }
      worker_pids[static_cast<size_t>(i)] = pid;
      std::cout << "[INFO] Started worker " << i << "/" << num_workers << " pid=" << pid << "\n";
    }
    std::cout << std::flush;

    // ---- Parent: inherited resources ----
    // The parent process does not handle FCGI requests — it only supervises
    // the children.  knxd_parent was already disconnected above; knxd_writer
    // is per-worker (each child reconnects its own copy).
    //
    // The parent KEEPS the listen socket open: every worker the supervisor
    // forks later inherits it.  Closing it here (as an earlier version did to
    // save one fd) makes each replacement worker fall back to spawn-fcgi mode,
    // exit immediately, and the pool can never recover from a crash.
    //
    // It also keeps the socket alive for the web server while no worker is
    // accepting, so connections queue in the backlog instead of failing.

    if (fork_failed) {
      // Kill all successfully started children
      std::cerr << "[ERROR] fork() failed, terminating all started children\n";
      if (cache_reader_pid > 0) {
        ::kill(cache_reader_pid, SIGTERM);
      }
      for (pid_t pid : worker_pids) {
        if (pid > 0) {
          ::kill(pid, SIGTERM);
        }
      }
      // Wait for killed children
      int status = 0;
      while (::wait(&status) > 0) {
      }
      result = 1;
    } else {
      std::cout << "[INFO] All " << num_workers << " workers started, supervising children...\n";
      std::cout << std::flush;

      // ---- Supervisor ----
      // The parent is the ONLY process that reaps children.  The SIGCHLD
      // handler must not call waitpid(): doing so steals the exit statuses
      // from this loop, so dead children were never noticed and never
      // replaced — the pool silently shrank to nothing over weeks.
      //
      // Every dead child is logged and restarted.  A dead cache reader means
      // no telegram ever reaches the shared cache again (the /r endpoint
      // would keep serving stale values forever), so it is restarted too.
      //
      // Restarts are rate-limited: a child that dies right after starting is
      // restarted with a growing delay (100 ms … 30 s) instead of spinning in
      // a fork/crash loop.
      struct RestartBackoff {
        int delay_ms = 100;
        std::chrono::steady_clock::time_point started_at = std::chrono::steady_clock::now();
        void on_spawn() { started_at = std::chrono::steady_clock::now(); }
        [[nodiscard]] int on_death() {
          const auto lived = std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::steady_clock::now() - started_at);
          if (lived.count() > 60) {
            delay_ms = 100;  // it had been running fine — no crash loop
          }
          const int wait_ms = delay_ms;
          delay_ms = std::min(delay_ms * 2, 30000);
          return wait_ms;
        }
      };

      RestartBackoff reader_backoff;
      std::vector<RestartBackoff> worker_backoff(static_cast<size_t>(num_workers));

      // A worker that died while serving a request took its semaphore tokens
      // with it.  Restore at most one token per semaphore and never exceed the
      // initial capacity, so a crash cannot permanently shrink the pool.
      const auto restore_semaphore = [num_workers](sem_t* sem) {
        if (sem == nullptr) {
          return;
        }
        int value = 0;
        if (::sem_getvalue(sem, &value) == 0 && value < num_workers) {
          ::sem_post(sem);
        }
      };

      while (g_shutdown_requested == 0) {
        int status = 0;
        const pid_t waited = ::wait(&status);
        if (waited < 0) {
          if (errno == EINTR) {
            continue;  // interrupted by a signal — check the shutdown flag
          }
          break;  // ECHILD: no more children
        }

        if (WIFEXITED(status)) {
          const int exit_code = WEXITSTATUS(status);
          if (exit_code != 0) {
            std::cerr << "[WARN] Child pid=" << waited << " exited with code " << exit_code << "\n";
          }
        } else if (WIFSIGNALED(status)) {
          std::cerr << "[WARN] Child pid=" << waited << " killed by signal " << WTERMSIG(status);
          if (WCOREDUMP(status)) {
            std::cerr << " (core dumped)";
          }
          std::cerr << "\n";
        }
        std::cerr << std::flush;

        if (g_shutdown_requested != 0) {
          break;  // shutting down — do not restart anything
        }

        if (waited == cache_reader_pid) {
          std::cerr << "[ERROR] Cache reader pid=" << waited << " died — restarting it\n";
          ::usleep(static_cast<useconds_t>(reader_backoff.on_death()) * 1000);
          cache_reader_pid = spawn_cache_reader();
          if (cache_reader_pid < 0) {
            std::cerr << "[ERROR] fork() for cache reader failed: " << std::strerror(errno) << "\n";
          } else {
            reader_backoff.on_spawn();
            std::cout << "[INFO] Restarted cache reader pid=" << cache_reader_pid << "\n";
            std::cout << std::flush;
          }
          continue;
        }

        for (size_t i = 0; i < worker_pids.size(); ++i) {
          if (worker_pids[i] != waited) {
            continue;
          }
          restore_semaphore(concurrency_sem);
          restore_semaphore(load_shed_sem);

          std::cerr << "[WARN] Worker " << i << " (pid=" << waited << ") died — restarting it\n";
          ::usleep(static_cast<useconds_t>(worker_backoff[i].on_death()) * 1000);
          worker_pids[i] = spawn_worker(static_cast<int>(i));
          if (worker_pids[i] < 0) {
            std::cerr << "[ERROR] fork() for worker " << i << " failed: " << std::strerror(errno)
                      << "\n";
          } else {
            worker_backoff[i].on_spawn();
            std::cout << "[INFO] Restarted worker " << i << " pid=" << worker_pids[i] << "\n";
            std::cout << std::flush;
          }
          break;
        }
      }

      // Shutdown requested: kill remaining children and wait for them
      if (g_shutdown_requested != 0) {
        std::cout << "[INFO] Shutdown requested, stopping all children...\n";
        std::cout << std::flush;
        if (cache_reader_pid > 0) {
          ::kill(cache_reader_pid, SIGTERM);
        }
        for (pid_t pid : worker_pids) {
          if (pid > 0) {
            ::kill(pid, SIGTERM);
          }
        }
        int status = 0;
        while (::wait(&status) > 0) {
        }
      }
      result = 0;
    }

    // ---- Clean up shared semaphores ----
    if (load_shed_sem != nullptr) {
      ::sem_destroy(load_shed_sem);
      ::munmap(load_shed_sem, sizeof(sem_t));
    }
    if (concurrency_sem != nullptr) {
      ::sem_destroy(concurrency_sem);
      ::munmap(concurrency_sem, sizeof(sem_t));
    }
  } else {
    // Spawn-fcgi mode: concurrency is handled by running multiple
    // process instances via spawn-fcgi (e.g., spawn-fcgi -F N).
    result = server.run();
  }

  // Cleanup
  // knxd_parent was already disconnected above; knxd_writer is per-worker.
  return result;
}
