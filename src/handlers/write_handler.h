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

#include <string>
#include <string_view>

namespace cvknxd {

class KnxdClientInterface;
class SessionStore;

/// Result of a write operation.
struct WriteResult {
  /// HTTP status code (200, 400, 401, 403, 404).
  int http_status = 200;
  /// Response body (always empty for write).
  std::string body;
};

/// Handles CometVisu write requests: GET /w?s=SESSION&a=ADDRESS&v=VALUE
class WriteHandler {
public:
  WriteHandler(KnxdClientInterface& knxd, SessionStore& sessions);

  /// Process a write request.
  /// @param query_string Raw QUERY_STRING from FCGI.
  /// @return WriteResult with status code.
  [[nodiscard]] WriteResult handle(std::string_view query_string);

private:
  KnxdClientInterface& knxd_;
  SessionStore& sessions_;
};

}  // namespace cvknxd
