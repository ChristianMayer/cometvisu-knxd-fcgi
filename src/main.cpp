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

#include <charconv>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "fcgi/fcgi_server.h"
#include "knxd/knxd_client.h"
#include "knxd/knxd_protocol.h"
#include "router/router.h"
#include "state/session_store.h"
#include "util/debug_log.h"
#include "version.h"

namespace {

/// Maximum long-poll timeout in seconds. Prevents DoS via huge timeout values.
inline constexpr int kMaxLongpollTimeoutSec = 3600;  // 1 hour

/// Read an environment variable with a default value.
const char* get_env_default(const char* name, const char* default_value) {
  const char* val = getenv(name);
  return (val != nullptr && val[0] != '\0') ? val : default_value;
}

/// Parse an integer from an environment variable safely using std::from_chars.
/// Returns the parsed value clamped to [min_val, max_val], or default_val on error.
int parse_env_int(const char* name, int default_val, int min_val, int max_val) {
  const char* val = getenv(name);
  if (val == nullptr || val[0] == '\0')
    return default_val;
  int result = 0;
  auto [ptr, ec] = std::from_chars(val, val + std::strlen(val), result);
  if (ec != std::errc{} || result < min_val)
    return default_val;
  if (result > max_val)
    return max_val;
  return result;
}

}  // namespace

int main(int argc, char* argv[]) {
  using namespace cvknxd;

  // Handle command-line flags when invoked directly (e.g., for version queries).
  // FastCGI binaries can be run from a terminal for introspection without
  // needing a running web server.
  if (argc > 1) {
    std::string_view arg = argv[1];
    if (arg == "--version" || arg == "-v") {
      std::cout << application_name() << " " << version() << "\n";
      return 0;
    }
    if (arg == "--help" || arg == "-h") {
      std::cout
          << application_name() << " " << version() << "\n"
          << "FastCGI backend bridging CometVisu Protocol to knxd\n"
          << "\n"
          << "Usage: cometvisu-knxd-fcgi [OPTION]\n"
          << "\n"
          << "Options:\n"
          << "  --version, -v   Print version information and exit\n"
          << "  --help, -h      Print this help message and exit\n"
          << "\n"
          << "Environment variables:\n"
          << "  KNXD_SOCKET           Path to the knxd Unix socket (default: /run/knx)\n"
          << "  FCGI_SOCKET           Direct FCGI socket (unset = spawn-fcgi mode)\n"
          << "  FCGI_THREADS          Worker threads in direct socket mode (default: 20, "
             "max: 256)\n"
          << "  LONGPOLL_TIMEOUT_SEC  Max seconds to wait in long-poll /r (default: 300)\n"
          << "  ADDRESS_PREFIX        Namespace prefix for addresses without explicit prefix\n"
          << "                        (default: empty = no prefix, e.g. set to \"KNX\")\n"
          << "  DEBUG_BACKEND         Set to 1 to enable debug logging to stderr\n"
          << "\n"
          << "When run without options, starts the FastCGI server loop.\n";
      return 0;
    }
  }

  // ---- Debug mode ----
  // Enable via environment variable DEBUG_BACKEND=1 (or DEBUG_BACKEND=true/yes/on).
  // When enabled, all HTTP request/response cycles and knxd communication
  // are printed to stderr for troubleshooting.
  DebugLog::init_from_env();

  // ---- Configuration ----
  // Environment variables are inherited from the FCGI-spawning web server.
  // This is safe because:
  //   - The web server (e.g. Apache, nginx, lighttpd) is the trusted parent process.
  //   - The knxd socket is a local Unix socket — redirection only affects the local machine.
  //   - An attacker who can manipulate the web server's environment already has
  //     sufficient access to compromise the system directly.
  const char* knxd_socket = get_env_default("KNXD_SOCKET", "/run/knx");
  int longpoll_timeout = parse_env_int("LONGPOLL_TIMEOUT_SEC", 300, 1, kMaxLongpollTimeoutSec);
  int fcgi_threads = parse_env_int("FCGI_THREADS", 20, 1, 256);

  // ---- Address prefix (namespace) ----
  // When the client sends addresses without a namespace prefix (no colon),
  // this value is used as the default namespace. Default: empty (no prefix).
  // Set ADDRESS_PREFIX=KNX to restore the old "KNX:1/2/3" format.
  const char* address_prefix = get_env_default("ADDRESS_PREFIX", "");
  KnxAddress::set_default_namespace(address_prefix);

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

  // ---- Optional direct socket (standalone mode) ----
  // Set FCGI_SOCKET to a TCP port (":9000") or Unix socket path to run without
  // spawn-fcgi. When unset, the server uses the standard FCGI stdin/stdout
  // stream set up by spawn-fcgi or the web server.
  const char* fcgi_socket = get_env_default("FCGI_SOCKET", "");
  if (fcgi_socket[0] != '\0') {
    if (server.listen(fcgi_socket)) {
      std::cout << "[INFO] Direct FCGI socket: " << fcgi_socket << "\n";
    } else {
      std::cerr << "[ERROR] Failed to open FCGI socket: " << fcgi_socket << "\n";
      knxd.disconnect();
      return 1;
    }
  }

  std::cout << "[INFO] cometvisu-knxd-fcgi starting, knxd socket: " << knxd_socket << "\n";

  // ---- Run ----
  int result;
  if (fcgi_socket[0] != '\0') {
    // Direct socket mode: use multi-threaded accept loop to handle
    // concurrent clients (including long-poll /r requests).
    std::cout << "[INFO] Using " << fcgi_threads << " worker threads\n";
    result = server.run_multithreaded(fcgi_threads);
  } else {
    // Spawn-fcgi mode: concurrency is handled by running multiple
    // process instances via spawn-fcgi (e.g., spawn-fcgi -F N).
    result = server.run();
  }

  // Cleanup
  knxd.disconnect();

  return result;
}
