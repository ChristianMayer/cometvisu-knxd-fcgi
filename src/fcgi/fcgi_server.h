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

#ifndef COMETVISU_KNXD_FCGI_FCGI_SERVER_H_
#define COMETVISU_KNXD_FCGI_FCGI_SERVER_H_

#include <fcgiapp.h>
#include <semaphore.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fcgi_request.h"

namespace cvknxd {

/// Callback type for request handlers.
/// Takes a parsed request and returns the response.
using RequestHandler = std::function<FcgiResponse(const FcgiRequest&)>;

/// Main FastCGI server: accepts requests from the web server and dispatches them.
/// Uses the libfcgi library for the FCGI protocol implementation.
///
/// Supports two modes:
///   1. Spawn-fcgi mode (default): reads/writes FCGI on stdin/stdout as set up
///      by spawn-fcgi or a web server. In this mode, concurrency is handled by
///      running multiple process instances via spawn-fcgi.
///   2. Direct socket mode: call listen() to open a TCP or Unix socket for
///      direct FCGI connections.
///      - run() accepts from the socket (single accept loop; each
///        call handles one client).  This is used by the fork-based
///        worker pool in main.cpp — each child process calls run().
///      - run_multithreaded() uses multiple std::thread workers, each
///        running its own FCGX_Accept_r() on the shared listen socket.
///   Note: main.cpp uses a fork-based worker pool (each child calls run())
///   instead of run_multithreaded(), for compatibility with Docker < 20.10.10
///   where seccomp blocks the clone3 syscall needed by std::thread.
class FcgiServer {
public:
  FcgiServer();
  ~FcgiServer();

  // Non-copyable, non-movable (holds threads and socket fd).
  FcgiServer(const FcgiServer&) = delete;
  FcgiServer& operator=(const FcgiServer&) = delete;
  FcgiServer(FcgiServer&&) = delete;
  FcgiServer& operator=(FcgiServer&&) = delete;

  /// Set the callback for handling requests.
  void set_handler(RequestHandler handler);

  /// Set a shared semaphore for load shedding across forked workers.
  /// When set, read (long-poll) requests will try to acquire the semaphore
  /// before blocking. If the semaphore is exhausted (all workers busy),
  /// the request returns an empty response immediately, allowing write
  /// requests to be served by workers that finish quickly.
  /// @param sem Pointer to a sem_t in shared memory (MAP_SHARED). Must be
  ///            initialized to the number of workers before forking.
  void set_load_shed_semaphore(sem_t* sem);

  /// Open a TCP or Unix socket for direct FCGI connections.
  /// Once opened, the socket is automatically used by run() alongside the
  /// standard FCGI stdin/stdout stream.
  ///
  /// @param socket_path Either ":port" for TCP (e.g., ":9000") or a
  ///                    filesystem path for a Unix domain socket.
  /// @param backlog Maximum queue length for pending connections (default: 128).
  /// @return true if the socket was opened successfully.
  [[nodiscard]] bool listen(const std::string& socket_path, int backlog = 128);

  /// Check if a listening socket has been opened.
  [[nodiscard]] bool is_listening() const;

  /// Close the listening socket. Safe to call from the parent process
  /// after fork() — the children retain their inherited copy.
  void close_listen_socket();

  /// Run the FCGI accept loop (single worker).  Blocks until the server
  /// shuts down or the listen socket is closed.  Each call handles one
  /// client at a time — for concurrent clients, run multiple workers via
  /// the fork-based pool in main.cpp, or use run_multithreaded().
  /// @return 0 on success, non-zero on error.
  int run();

  /// Run the FCGI accept loop with multiple std::thread workers.
  /// Each thread runs its own FCGX_Accept_r() on the shared listen socket.
  /// The OS serializes accept() calls across threads, allowing multiple
  /// concurrent clients to be served independently.
  ///
  /// This method uses std::thread internally.  On systems where thread
  /// creation is blocked (e.g. Docker < 20.10.10 with glibc >= 2.34),
  /// use the fork-based worker pool in main.cpp instead.
  ///
  /// Blocks until shutdown() is called from another thread.
  /// @param num_threads Number of worker threads (minimum 1).
  /// @return 0 on success, non-zero on error.
  int run_multithreaded(int num_threads);

  /// Request shutdown of the accept loop(s).  Safe to call from any
  /// thread, signal handler, or parent process.
  /// This causes run() and run_multithreaded() to return.
  void shutdown();

private:
  RequestHandler handler_;
  int listen_fd_ = -1;
  FCGX_Request request_{};
  std::atomic<bool> shutdown_requested_{false};
  std::vector<std::thread> workers_;
  int num_workers_ = 0;             // set by run_multithreaded(), used by shutdown()
  std::mutex fcgi_mutex_;           // serializes libfcgi calls in multithreaded mode
  sem_t* load_shed_sem_ = nullptr;  // shared semaphore for load shedding

  /// Read all FCGI parameters from stdin into an FcgiRequest.
  /// Uses getenv() which reads from the global environ pointer.
  [[nodiscard]] static FcgiRequest read_request();

  /// Read FCGI parameters from an explicit FCGI parameter array into an
  /// FcgiRequest.  Thread-safe: uses the passed envp directly instead of
  /// the global environ pointer, avoiding races when multiple threads
  /// would overwrite environ concurrently.
  /// @param envp  FCGI parameter array (FCGX_Request::envp).
  [[nodiscard]] static FcgiRequest read_request(char** envp);
  /// Write an FcgiResponse to the appropriate output stream.
  /// Uses FCGX_Request::out when in direct socket mode, FCGI stdout otherwise.
  void write_response(const FcgiResponse& response);
  /// Write an FcgiResponse to a specific FCGX_Request output stream.
  /// Used by both worker threads (run_multithreaded) and child
  /// processes (fork-based pool in main.cpp).
  static void write_response_direct(FCGX_Request& request, const FcgiResponse& response);
};

}  // namespace cvknxd

#endif  // COMETVISU_KNXD_FCGI_FCGI_SERVER_H_
