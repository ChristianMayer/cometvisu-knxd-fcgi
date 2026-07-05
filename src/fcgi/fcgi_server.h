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

#include <functional>
#include <string>

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
///      by spawn-fcgi or a web server.
///   2. Direct socket mode: call listen() to open a TCP or Unix socket for
///      direct FCGI connections. run() accepts from the socket instead of stdin.
class FcgiServer {
public:
  FcgiServer();

  /// Set the callback for handling requests.
  void set_handler(RequestHandler handler);

  /// Open a TCP or Unix socket for direct FCGI connections.
  /// Once opened, the socket is automatically used by run() alongside the
  /// standard FCGI stdin/stdout stream.
  ///
  /// @param socket_path Either ":port" for TCP (e.g., ":9000") or a
  ///                    filesystem path for a Unix domain socket.
  /// @return true if the socket was opened successfully.
  [[nodiscard]] bool listen(const std::string& socket_path);

  /// Check if a listening socket has been opened.
  [[nodiscard]] bool is_listening() const;

  /// Run the FCGI accept loop. Blocks until the server shuts down.
  /// @return 0 on success, non-zero on error.
  int run();

private:
  RequestHandler handler_;
  int listen_fd_ = -1;
  FCGX_Request request_{};

  /// Read all FCGI parameters from stdin into an FcgiRequest.
  [[nodiscard]] static FcgiRequest read_request();
  /// Write an FcgiResponse to the appropriate output stream.
  /// Uses FCGX_Request::out when in direct socket mode, FCGI stdout otherwise.
  void write_response(const FcgiResponse& response);
};

}  // namespace cvknxd
