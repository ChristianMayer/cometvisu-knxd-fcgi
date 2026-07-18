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

#include "router.h"

#include "../util/query_string.h"

namespace cvknxd {

Router::Router(KnxdClientInterface& knxd, SessionStore& sessions, int longpoll_timeout_sec,
               std::string base_url)
    : login_handler_(sessions, std::move(base_url)),
      read_handler_(knxd, sessions, longpoll_timeout_sec),
      write_handler_(knxd, sessions) {}

FcgiResponse Router::route(const FcgiRequest& request) {
  FcgiResponse response;

  // Use FcgiRequest::path() to get the clean path without query string
  const std::string_view path = request.path();

  if (path == "/l") {
    const std::string body = login_handler_.handle(request.query_string);
    response.body = std::move(body);
  } else if (path == "/r") {
    const auto result = read_handler_.handle(request.query_string);
    response.status_code = result.http_status;
    response.body = std::move(result.body);
  } else if (path == "/w") {
    const auto result = write_handler_.handle(request.query_string);
    response.status_code = result.http_status;
  } else {
    // Unknown endpoint
    response.status_code = 404;
    response.body = R"({"error":"unknown endpoint"})";
  }

  return response;
}

}  // namespace cvknxd
