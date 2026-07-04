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

#include "fcgi_request.h"

namespace cvknxd {

std::string_view FcgiRequest::path() const {
  // path_info is the primary source for the endpoint path
  // (e.g. SCRIPT_NAME=/cgi-bin/visu, PATH_INFO=/l)
  if (!path_info.empty())
    return std::string_view{path_info};

  // Fall back to request_uri minus query string
  if (request_uri.empty())
    return {};
  auto qpos = request_uri.find('?');
  if (qpos == std::string::npos) {
    return std::string_view{request_uri};
  }
  return std::string_view{request_uri.data(), qpos};
}

}  // namespace cvknxd
