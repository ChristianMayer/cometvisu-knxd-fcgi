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

#include <cstddef>
#include <string>
#include <string_view>

namespace cvknxd {

/// Debug logging facility for tracing HTTP ↔ app and app ↔ knxd communication.
///
/// Controlled via the `DEBUG_BACKEND` environment variable (set to "1" or "true").
/// Output goes to stderr with timestamps. Long URIs and large response bodies
/// are truncated for readability, with total size noted.
///
/// Methods are static for easy access from any component without injection.
/// All methods are no-ops when disabled, with minimal overhead (a single bool check).
class DebugLog {
public:
  /// Initialize from environment variable `DEBUG_BACKEND`.
  static void init_from_env();

  /// Enable or disable debug logging programmatically.
  static void set_enabled(bool enabled) { enabled_ = enabled; }

  /// Check if debug logging is enabled.
  [[nodiscard]] static bool is_enabled() { return enabled_; }

  /// Set the maximum characters to print for a request URI before truncation.
  /// Set to 0 for no truncation.
  static void set_max_uri_length(size_t max_len) { max_uri_length_ = max_len; }

  /// Set the maximum characters to print for a response body before truncation.
  /// Set to 0 for no truncation.
  static void set_max_body_length(size_t max_len) { max_body_length_ = max_len; }

  /// Log an incoming HTTP request.
  /// @param method HTTP method (GET, POST, etc.).
  /// @param uri Full request URI (may be truncated in output).
  static void http_request(std::string_view method, std::string_view uri);

  /// Log an outgoing HTTP response.
  /// @param status_code HTTP status code.
  /// @param body Response body (may be truncated in output).
  static void http_response(int status_code, std::string_view body);

  /// Log a knxd send operation.
  /// @param operation Name of the operation (e.g. "cache_read", "group_packet").
  /// @param address KNX group address string (e.g. "1/2/3").
  /// @param details Additional details (e.g. "nowait=true", "apdu=008042").
  static void knxd_send(std::string_view operation, std::string_view address,
                        std::string_view details);

  /// Log a knxd receive operation.
  /// @param operation Name of the operation (e.g. "cache_read", "apdu_packet").
  /// @param address KNX group address string (e.g. "1/2/3").
  /// @param data Hex-encoded data or other result info.
  static void knxd_recv(std::string_view operation, std::string_view address,
                        std::string_view data);

private:
  static bool enabled_;
  static size_t max_uri_length_;
  static size_t max_body_length_;

  /// Write a timestamp prefix to stderr.
  static void write_timestamp();

  /// Write a possibly-truncated string to stderr.
  /// Returns true if truncation occurred.
  static bool write_truncated(std::string_view text, size_t max_len);
};

}  // namespace cvknxd
