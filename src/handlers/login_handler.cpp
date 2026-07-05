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

#include "state/session_store.h"
#include "util/json_builder.h"
#include "util/query_string.h"

namespace cvknxd {

LoginHandler::LoginHandler(SessionStore& sessions) : sessions_(sessions) {}

std::string LoginHandler::handle(std::string_view query_string) {
  QueryString params{query_string};

  // Check if anonymous session
  bool anonymous = !params.has("u") && !params.has("p");

  std::string session_id = sessions_.create_session(anonymous);

  JsonBuilder json;
  json.start_object();
  json.add_string("v", "0.0.2");
  json.add_string("s", session_id);
  json.end_object();

  return json.take();
}

}  // namespace cvknxd
