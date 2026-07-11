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

#pragma once

#include <fcgiapp.h>

#include <atomic>
#include <functional>
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
///      - run() accepts from the socket in a single thread (legacy).
///      - run_multithreaded() uses multiple worker threads, each running
///        its own FCGX_Accept_r() on the shared listen socket. The OS
///        serializes accept() calls across threads. This is the preferred
///        mode for handling concurrent long-poll clients.
class FcgiServer {
public:
  FcgiServer();
  ~FcgiServer();

  /// Set the callback for handling requests.
  void set_handler(RequestHandler handler);

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

  /// Run the FCGI accept loop (single-threaded). Blocks until the server shuts down.
  /// Use run_multithreaded() for concurrent client handling.
  /// @return 0 on success, non-zero on error.
  int run();

  /// Run the FCGI accept loop with multiple worker threads.
  /// Each thread runs its own FCGX_Accept_r() on the shared listen socket.
  /// The OS serializes accept() calls across threads, allowing multiple
  /// concurrent clients to be served independently.
  /// Blocks until shutdown() is called from another thread.
  /// @param num_threads Number of worker threads (minimum 1).
  /// @return 0 on success, non-zero on error.
  int run_multithreaded(int num_threads);

  /// Request shutdown of the accept loop(s). Safe to call from any thread.
  /// This causes run() and run_multithreaded() to return.
  void shutdown();

private:
  RequestHandler handler_;
  int listen_fd_ = -1;
  FCGX_Request request_{};
  std::atomic<bool> shutdown_requested_{false};
  std::vector<std::thread> workers_;
  int num_workers_ = 0;  // set by run_multithreaded(), used by shutdown()

  /// Read all FCGI parameters from stdin into an FcgiRequest.
  [[nodiscard]] static FcgiRequest read_request();
  /// Write an FcgiResponse to the appropriate output stream.
  /// Uses FCGX_Request::out when in direct socket mode, FCGI stdout otherwise.
  void write_response(const FcgiResponse& response);
  /// Write an FcgiResponse to a specific FCGX_Request output stream.
  /// Used by worker threads in multithreaded mode.
  static void write_response_direct(FCGX_Request& request, const FcgiResponse& response);
};

}  // namespace cvknxd
