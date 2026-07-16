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

#ifndef COMETVISU_KNXD_FCGI_WRITE_HANDLER_H_
#define COMETVISU_KNXD_FCGI_WRITE_HANDLER_H_

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

  ~WriteHandler() = default;

  // Reference members prevent copy/move.
  WriteHandler(const WriteHandler&) = delete;
  WriteHandler& operator=(const WriteHandler&) = delete;
  WriteHandler(WriteHandler&&) = delete;
  WriteHandler& operator=(WriteHandler&&) = delete;

  /// Process a write request.
  /// @param query_string Raw QUERY_STRING from FCGI.
  /// @return WriteResult with status code.
  [[nodiscard]] WriteResult handle(std::string_view query_string);

private:
  KnxdClientInterface& knxd_;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
  SessionStore& sessions_;     // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
};

}  // namespace cvknxd

#endif  // COMETVISU_KNXD_FCGI_WRITE_HANDLER_H_
