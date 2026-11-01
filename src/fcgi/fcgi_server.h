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

/**
 * @file fcgi_server.h
 * @brief FastCGI server — accept loop, request parsing, and response writing.
 *
 * Wraps the libfcgi library (both the high-level FCGI_stdio API used in
 * spawn-fcgi mode and the lower-level FCGX API used in direct-socket mode).
 *
 * The original eibread-cgi / eibwrite-cgi reference implementations used the
 * simpler FCGI_stdio API exclusively and ran under spawn-fcgi.  This class
 * extends that model with a direct-socket mode that supports a fork-based
 * worker pool without requiring spawn-fcgi.
 */

#include <fcgiapp.h>
#include <semaphore.h>

#include <atomic>
#include <functional>
#include <string>

#include "fcgi_request.h"

namespace cvknxd {

/// Callback type for request handlers.
/// Takes a parsed request and returns the response.
using RequestHandler = std::function<FcgiResponse(const FcgiRequest&)>;

/**
 * @brief Main FastCGI server: accepts requests and dispatches them to handlers.
 *
 * Uses the libfcgi library for the FCGI protocol implementation.
 *
 * Supports two operating modes:
 *   1. **Spawn-fcgi mode** (default): reads/writes FCGI on stdin/stdout as
 *      configured by spawn-fcgi or a web server.  Concurrency is achieved by
 *      running multiple process instances via spawn-fcgi's `-F` flag.
 *      This is the mode used by the reference eibread-cgi / eibwrite-cgi.
 *   2. **Direct socket mode**: call listen() to open a TCP or Unix socket,
 *      then run() to accept connections.  For concurrent clients, main.cpp
 *      uses a fork-based worker pool — each child process calls run() on the
 *      inherited listen fd.
 *
 * @note In direct socket mode, FCGX_Init() must be called before
 *       FCGX_Accept_r().  FCGX_InitRequest() alone does not set the
 *       libInitialized flag, causing FCGX_Accept_r() to return -9998.
 */
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

  /// @brief Set a shared semaphore for load shedding across forked workers.
  ///
  /// When set, read (long-poll) requests will try to acquire the semaphore
  /// before blocking.  If the semaphore is exhausted (all workers busy),
  /// the request returns HTTP 503 immediately, allowing write requests to
  /// be served by workers that finish quickly.
  ///
  /// @param sem Pointer to a sem_t in shared memory (MAP_SHARED).  Must be
  ///            initialized to the number of workers before forking.
  void set_load_shed_semaphore(sem_t* sem);

  /// @brief Set a shared semaphore for general request concurrency limiting.
  ///
  /// When set, long-poll read (`/r`) requests must acquire this semaphore
  /// before processing.  If exhausted, the request returns HTTP 503
  /// immediately, preventing the listen backlog from filling up and causing
  /// the reverse proxy to return 502 Bad Gateway.
  ///
  /// Writes (`/w`) and logins (`/l`) never take this token: they finish in
  /// microseconds, and a spurious 503 would be reported to the user as a
  /// failed operation.
  ///
  /// @param sem Pointer to a sem_t in shared memory (MAP_SHARED).  Must be
  ///            initialized to a safe concurrency limit (typically equal to
  ///            the worker count).
  void set_concurrency_semaphore(sem_t* sem);

  /// @brief Open a TCP or Unix socket for direct FCGI connections.
  ///
  /// Once opened, the socket is automatically used by run() alongside the
  /// standard FCGI stdin/stdout stream.
  ///
  /// @param socket_path Either ":port" for TCP (e.g. ":9000") or a
  ///                    filesystem path for a Unix domain socket.
  /// @param backlog Maximum queue length for pending connections (default: 128).
  /// @return true if the socket was opened successfully.
  [[nodiscard]] bool listen(const std::string& socket_path, int backlog = 128);

  /// Check if a listening socket has been opened.
  [[nodiscard]] bool is_listening() const;

  /// @brief Close the listening socket.
  ///
  /// Safe to call from the parent process after fork() — the children
  /// retain their inherited copy and continue to accept connections.
  void close_listen_socket();

  /// @brief Run the FCGI accept loop (single worker).
  ///
  /// Blocks until the server shuts down or the listen socket is closed.
  /// Each call handles one client at a time — for concurrent clients, run
  /// multiple workers via the fork-based pool in main.cpp.
  ///
  /// @return 0 on success, non-zero on error.
  int run();

  /// @brief Request shutdown of the accept loop(s).
  ///
  /// Safe to call from any thread, signal handler, or parent process.
  /// Causes run() to return.  Uses shutdown(SHUT_RDWR) + self-connect
  /// to reliably unblock workers stuck in accept().
  void shutdown();

private:
  RequestHandler handler_;
  int listen_fd_ = -1;
  FCGX_Request request_{};
  std::atomic<bool> shutdown_requested_{false};
  sem_t* load_shed_sem_ = nullptr;    // shared semaphore for load shedding (/r only)
  sem_t* concurrency_sem_ = nullptr;  // shared semaphore for long-poll (/r only)

  /// Invoke the request handler, converting an exception into HTTP 500.
  ///
  /// A throwing handler must not terminate the worker process: that would
  /// kill every other request this worker is queued for and permanently lose
  /// the semaphore capacity the worker held.
  [[nodiscard]] FcgiResponse invoke_handler(const FcgiRequest& request);

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
};

}  // namespace cvknxd

#endif  // COMETVISU_KNXD_FCGI_FCGI_SERVER_H_
