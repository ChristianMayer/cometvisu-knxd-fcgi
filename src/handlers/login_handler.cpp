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

#include "login_handler.h"

#include <cstdio>

#include "state/session_store.h"
#include "util/json_builder.h"
#include "util/query_string.h"
#include "version.h"

namespace cvknxd {

/// CometVisu protocol version reported in the login response "v" field.
/// This is the wire-protocol version, not the software version.
static constexpr std::string_view kProtocolVersion = "0.0.2";

LoginHandler::LoginHandler(SessionStore& sessions, std::string base_url)
    : sessions_(sessions), base_url_(std::move(base_url)) {}

std::string LoginHandler::query_knxd_version() {
  // Run `knxd --version` and return its raw output (trimmed).
  // knxd may print version info to stderr, so merge both streams.
  FILE* pipe = popen("knxd --version 2>&1", "r");
  if (!pipe)
    return "";

  char buf[256];
  std::string output;
  while (std::fgets(buf, sizeof(buf), pipe) != nullptr) {
    output += buf;
  }
  pclose(pipe);

  // Trim trailing whitespace / newlines
  while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
    output.pop_back();
  }
  return output;
}

std::string LoginHandler::handle(std::string_view query_string) {
  const QueryString params{query_string};

  const bool anonymous = !params.has("u") && !params.has("p");
  const std::string session_id = sessions_.create_session(anonymous);

  JsonBuilder json;
  json.start_object();
  json.add_string("v", kProtocolVersion);
  json.add_string("s", session_id);

  // Gather version info (compile-time + runtime, lazy cached)
  bool need_config = !base_url_.empty();
  const std::string_view fcgi_ver = version();
  const std::string_view fcgi_hash = git_hash();
  const std::string_view knxd_bver = knxd_build_version();
  const std::string_view knxd_bhash = knxd_build_git_hash();

  if (!fcgi_ver.empty() && fcgi_ver != "unknown")
    need_config = true;
  if (!fcgi_hash.empty() && fcgi_hash != "unknown")
    need_config = true;
  if (!knxd_bver.empty())
    need_config = true;
  if (!knxd_bhash.empty())
    need_config = true;

  if (cached_knxd_runtime_.empty()) {
    cached_knxd_runtime_ = query_knxd_version();
  }
  if (!cached_knxd_runtime_.empty())
    need_config = true;

  if (need_config) {
    json.add_key("c");
    json.start_object();

    if (!base_url_.empty()) {
      json.add_string("baseURL", base_url_);
    }

    if (!fcgi_ver.empty() && fcgi_ver != "unknown") {
      json.add_string("fcgiVersion", fcgi_ver);
    }
    if (!fcgi_hash.empty() && fcgi_hash != "unknown") {
      json.add_string("fcgiGitHash", fcgi_hash);
    }
    if (!knxd_bver.empty()) {
      json.add_string("knxdBuildVersion", knxd_bver);
    }
    if (!knxd_bhash.empty()) {
      json.add_string("knxdBuildGitHash", knxd_bhash);
    }
    if (!cached_knxd_runtime_.empty()) {
      json.add_string("knxdRuntimeVersion", cached_knxd_runtime_);
    }

    json.end_object();
  }

  json.end_object();

  return json.take();
}

}  // namespace cvknxd
