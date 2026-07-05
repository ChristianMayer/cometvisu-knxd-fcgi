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

class SessionStore;

/// Handles CometVisu login requests: GET /l?u=USER&p=PASSWORD&d=DEVICE
class LoginHandler {
public:
  explicit LoginHandler(SessionStore& sessions);

  /// Process a login request.
  /// @param query_string Raw QUERY_STRING from FCGI.
  /// @return JSON response body: {"v":"0.0.2","s":"SESSION_ID"}
  [[nodiscard]] std::string handle(std::string_view query_string);

private:
  SessionStore& sessions_;
};

}  // namespace cvknxd
