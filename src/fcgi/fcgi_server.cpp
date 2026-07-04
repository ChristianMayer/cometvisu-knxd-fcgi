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

#include "fcgi_server.h"

#include <fcgi_stdio.h>

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace cvknxd {

FcgiServer::FcgiServer() = default;

void FcgiServer::set_handler(RequestHandler handler) {
  handler_ = std::move(handler);
}

int FcgiServer::run() {
  if (!handler_) {
    std::cerr << "[ERROR] No request handler set\n";
    return 1;
  }

  while (FCGI_Accept() >= 0) {
    FcgiRequest req = read_request();
    FcgiResponse resp = handler_(req);
    write_response(resp);
  }

  return 0;
}

FcgiRequest FcgiServer::read_request() {
  FcgiRequest req;

  // Read FCGI environment variables
  auto get_env = [](const char* name) -> const char* {
    const char* val = getenv(name);
    return val ? val : "";
  };

  req.request_method = get_env("REQUEST_METHOD");
  req.request_uri = get_env("REQUEST_URI");
  req.query_string = get_env("QUERY_STRING");
  req.content_type = get_env("CONTENT_TYPE");
  req.script_name = get_env("SCRIPT_NAME");
  req.path_info = get_env("PATH_INFO");
  req.server_protocol = get_env("SERVER_PROTOCOL");

  // Read request body (for POST data, e.g., filter operations)
  if (req.request_method == "POST" || req.request_method == "PUT") {
    const char* content_length_str = getenv("CONTENT_LENGTH");
    if (content_length_str[0] != '\0') {
      int content_length = std::atoi(content_length_str);
      if (content_length > 0) {
        req.content.resize(static_cast<size_t>(content_length));
        // Read from FCGI stdin
        size_t total = 0;
        while (total < static_cast<size_t>(content_length)) {
          int n = FCGI_fread(req.content.data() + total, 1,
                             static_cast<int>(req.content.size() - total), stdin);
          if (n <= 0)
            break;
          total += static_cast<size_t>(n);
        }
      }
    }
  }

  return req;
}

void FcgiServer::write_response(const FcgiResponse& response) {
  // Status header
  std::string status_line = "Status: " + std::to_string(response.status_code) + "\r\n";

  // Content-Type header
  std::string content_type_line = "Content-Type: " + response.content_type + "; charset=utf-8\r\n";

  // Content-Length header
  std::string content_length_line =
      "Content-Length: " + std::to_string(response.body.size()) + "\r\n";

  // Blank line separator
  std::string header_end = "\r\n";

  // Write to FCGI stdout.
  // Note: FCGI_fwrite takes non-const void* (C API), but does not modify the buffer.
  // The const_cast is safe here as the library only reads from the buffer.
  FCGI_fwrite(const_cast<char*>(status_line.data()), 1, status_line.size(), stdout);
  FCGI_fwrite(const_cast<char*>(content_type_line.data()), 1, content_type_line.size(), stdout);
  FCGI_fwrite(const_cast<char*>(content_length_line.data()), 1, content_length_line.size(), stdout);
  FCGI_fwrite(const_cast<char*>(header_end.data()), 1, header_end.size(), stdout);
  FCGI_fwrite(const_cast<char*>(response.body.data()), 1, response.body.size(), stdout);
}

}  // namespace cvknxd
