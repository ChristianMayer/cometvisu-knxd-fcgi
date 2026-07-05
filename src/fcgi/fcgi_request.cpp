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
  // PATH_INFO is the primary source for the endpoint path.
  // e.g. SCRIPT_NAME=/cgi-bin/visu, PATH_INFO=/l
  if (!path_info.empty())
    return std::string_view{path_info};

  // Fall back to request_uri minus query string
  std::string_view uri_path;
  if (!request_uri.empty()) {
    auto qpos = request_uri.find('?');
    uri_path = (qpos == std::string::npos) ? std::string_view{request_uri}
                                           : std::string_view{request_uri.data(), qpos};
  } else {
    return {};
  }

  // If SCRIPT_NAME is a prefix of uri_path, extract the trailing part.
  // e.g. SCRIPT_NAME=/cgi-bin/visu, uri_path=/cgi-bin/visu/l  →  /l
  if (!script_name.empty() && uri_path.size() >= script_name.size() &&
      uri_path.compare(0, script_name.size(), script_name) == 0) {
    std::string_view trail = uri_path.substr(script_name.size());
    if (!trail.empty())
      return trail;
    // SCRIPT_NAME consumes the entire URI path (e.g. SCRIPT_NAME=/cgi-bin/l,
    // uri_path=/cgi-bin/l).  Use the last path component of SCRIPT_NAME
    // itself as the endpoint identifier.
    auto last_slash = script_name.rfind('/');
    if (last_slash != std::string_view::npos) {
      return std::string_view{script_name}.substr(last_slash);
    }
  }

  return uri_path;
}

}  // namespace cvknxd
