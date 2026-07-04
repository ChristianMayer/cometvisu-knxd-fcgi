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

#include <cstdlib>
#include <iostream>
#include <string>

#include "fcgi/fcgi_server.h"
#include "knxd/knxd_client.h"
#include "router/router.h"
#include "state/session_store.h"

namespace {

/// Read an environment variable with a default value.
const char* get_env_default(const char* name, const char* default_value) {
  const char* val = getenv(name);
  return (val != nullptr && val[0] != '\0') ? val : default_value;
}

}  // namespace

int main() {
  using namespace cvknxd;

  // ---- Configuration ----
  const char* knxd_socket = get_env_default("KNXD_SOCKET", "/run/knx");
  int longpoll_timeout = 60;
  if (const char* lp = getenv("LONGPOLL_TIMEOUT_SEC")) {
    longpoll_timeout = std::atoi(lp);
    if (longpoll_timeout <= 0)
      longpoll_timeout = 60;
  }

  // ---- Initialize components ----
  KnxdClient knxd;
  if (!knxd.connect(knxd_socket)) {
    std::cerr << "[ERROR] Cannot connect to knxd at " << knxd_socket << "\n";
    return 1;
  }

  // Open group socket for sending and receiving
  if (!knxd.open_group_socket(false)) {
    std::cerr << "[ERROR] Cannot open group socket on knxd\n";
    return 1;
  }

  // Set non-blocking mode for efficient poll()-based long-poll
  knxd.set_nonblocking(true);

  SessionStore sessions;

  // ---- Create router and server ----
  // No local cache — we delegate to knxd's built-in cache via cache_read().
  Router router(knxd, sessions, longpoll_timeout);

  FcgiServer server;
  server.set_handler([&](const FcgiRequest& req) -> FcgiResponse { return router.route(req); });

  std::cout << "[INFO] cometvisu-knxd-fcgi starting, knxd socket: " << knxd_socket << "\n";

  // ---- Run ----
  int result = server.run();

  // Cleanup
  knxd.disconnect();

  return result;
}
