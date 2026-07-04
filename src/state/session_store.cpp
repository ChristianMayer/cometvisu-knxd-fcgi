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

#include "session_store.h"

#include <random>

namespace cvknxd {

SessionStore::SessionStore(int session_ttl_sec) : session_ttl_sec_(session_ttl_sec) {}

std::string SessionStore::create_session(bool anonymous) {
  if (anonymous)
    return "0";

  // Clean up expired sessions before creating a new one
  cleanup_expired();

  std::string id = generate_id();
  sessions_[id] = Session{id, std::chrono::steady_clock::now()};
  return id;
}

bool SessionStore::is_valid(std::string_view session_id) const {
  if (session_id == "0")
    return true;  // anonymous always valid
  std::string key{session_id};
  auto it = sessions_.find(key);
  if (it == sessions_.end())
    return false;

  // Check expiration
  auto age = std::chrono::steady_clock::now() - it->second.created;
  return std::chrono::duration_cast<std::chrono::seconds>(age).count() < session_ttl_sec_;
}

void SessionStore::remove(std::string_view session_id) {
  if (session_id == "0")
    return;
  std::string key{session_id};
  sessions_.erase(key);
}

void SessionStore::cleanup_expired() {
  auto now = std::chrono::steady_clock::now();
  auto it = sessions_.begin();
  while (it != sessions_.end()) {
    auto age = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.created).count();
    if (age >= session_ttl_sec_) {
      it = sessions_.erase(it);
    } else {
      ++it;
    }
  }
}

std::string SessionStore::generate_id() {
  // Cryptographically secure random session IDs
  static thread_local std::random_device rd;
  static thread_local std::mt19937_64 gen(rd());
  static thread_local std::uniform_int_distribution<uint64_t> dist;

  uint64_t num = dist(gen);
  std::string id;
  id.reserve(16);
  static constexpr char kHex[] = "0123456789abcdef";
  for (int i = 0; i < 16; ++i) {
    id.push_back(kHex[(num >> (60 - i * 4)) & 0xF]);
  }
  return id;
}

}  // namespace cvknxd
