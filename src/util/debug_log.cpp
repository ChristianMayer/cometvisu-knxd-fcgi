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

#include "debug_log.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace cvknxd {

bool DebugLog::enabled_ = false;
size_t DebugLog::max_uri_length_ = 500;
size_t DebugLog::max_body_length_ = 1000;

void DebugLog::init_from_env() {
  const char* val = std::getenv("DEBUG_BACKEND");
  if (val == nullptr || val[0] == '\0') {
    enabled_ = false;
    return;
  }
  // Accept "1", "true", "yes", "on" (case-insensitive prefix match)
  if (std::strcmp(val, "1") == 0 || std::strcmp(val, "true") == 0 || std::strcmp(val, "yes") == 0 ||
      std::strcmp(val, "on") == 0) {
    enabled_ = true;
  } else {
    enabled_ = false;
  }
}

void DebugLog::write_timestamp(std::ostream& os) {
  auto now = std::chrono::system_clock::now();
  auto time_t_now = std::chrono::system_clock::to_time_t(now);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

  std::tm tm_now;
  localtime_r(&time_t_now, &tm_now);

  os << "[" << std::put_time(&tm_now, "%Y-%m-%d %H:%M:%S") << "." << std::setfill('0')
     << std::setw(3) << ms.count() << "] ";
}

bool DebugLog::write_truncated(std::ostream& os, std::string_view text, size_t max_len) {
  if (max_len == 0 || text.size() <= max_len) {
    os << text;
    return false;
  }
  // Truncate: show first max_len chars
  os << text.substr(0, max_len) << "... (truncated, " << text.size() << " chars total)";
  return true;
}

void DebugLog::http_request(std::string_view method, std::string_view uri) {
  if (!enabled_)
    return;

  // Build the entire log line in a local buffer to write it atomically.
  // Multiple processes share stderr; building then writing in one call
  // minimises (but cannot completely eliminate) interleaving.
  std::ostringstream oss;
  write_timestamp(oss);
  oss << "→ HTTP REQUEST: " << method << " ";
  write_truncated(oss, uri, max_uri_length_);
  oss << "\n";
  std::cerr << oss.str() << std::flush;
}

void DebugLog::http_response(int status_code, std::string_view body) {
  if (!enabled_)
    return;

  std::ostringstream oss;
  write_timestamp(oss);
  oss << "← HTTP RESPONSE: " << status_code;
  if (!body.empty()) {
    oss << " body=";
    write_truncated(oss, body, max_body_length_);
  }
  oss << "\n";
  std::cerr << oss.str() << std::flush;
}

void DebugLog::knxd_send(std::string_view operation, std::string_view address,
                         std::string_view details) {
  if (!enabled_)
    return;

  std::ostringstream oss;
  write_timestamp(oss);
  oss << "  → KNXD SEND: " << operation << " addr=" << address;
  if (!details.empty()) {
    oss << " " << details;
  }
  oss << "\n";
  std::cerr << oss.str() << std::flush;
}

void DebugLog::knxd_recv(std::string_view operation, std::string_view address,
                         std::string_view data) {
  if (!enabled_)
    return;

  std::ostringstream oss;
  write_timestamp(oss);
  oss << "  ← KNXD RECV: " << operation << " addr=" << address;
  if (!data.empty()) {
    oss << " data=" << data;
  }
  oss << "\n";
  std::cerr << oss.str() << std::flush;
}

}  // namespace cvknxd
