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

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace cvknxd {

/// Stores active CometVisu sessions with their metadata.
/// Sessions expire after a configurable TTL (default: 30 minutes).
/// Thread-safe: no (single-threaded process).
class SessionStore {
public:
  /// Default session TTL in seconds.
  static constexpr int kDefaultSessionTtlSec = 1800;  // 30 minutes

  explicit SessionStore(int session_ttl_sec = kDefaultSessionTtlSec);

  /// Create a new session. Returns the session ID.
  /// @param anonymous If true, creates an anonymous session (ID = "0").
  [[nodiscard]] std::string create_session(bool anonymous = false);

  /// Check if a session exists and is valid (not expired).
  [[nodiscard]] bool is_valid(std::string_view session_id) const;

  /// Remove a session.
  void remove(std::string_view session_id);

  /// Remove all expired sessions. Called automatically on create_session().
  void cleanup_expired();

  /// Get the number of active (non-expired) sessions.
  [[nodiscard]] size_t count() const { return sessions_.size(); }

private:
  struct Session {
    std::string id;
    std::chrono::steady_clock::time_point created;
  };

  std::unordered_map<std::string, Session> sessions_;
  int session_ttl_sec_;

  [[nodiscard]] std::string generate_id();
};

}  // namespace cvknxd
