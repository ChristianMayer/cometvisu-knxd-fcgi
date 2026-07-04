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

#include "fcgi/fcgi_request.h"
#include "fcgi/fcgi_server.h"
#include "handlers/login_handler.h"
#include "handlers/read_handler.h"
#include "handlers/write_handler.h"

namespace cvknxd {

/// URL router: dispatches FCGI requests to the appropriate handler.
class Router {
public:
  Router(KnxdClientInterface& knxd, SessionStore& sessions, int longpoll_timeout_sec = 60);

  /// Dispatch a request and return the response.
  [[nodiscard]] FcgiResponse route(const FcgiRequest& request);

private:
  LoginHandler login_handler_;
  ReadHandler read_handler_;
  WriteHandler write_handler_;
};

}  // namespace cvknxd
