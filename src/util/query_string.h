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

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cvknxd {

/// Transparent hash and equality for heterogeneous lookup with string_view
/// in unordered_map<string, ...> containers (C++20).
struct StringHash {
  using is_transparent = void;  // enables heterogeneous lookup

  [[nodiscard]] size_t operator()(std::string_view sv) const {
    return std::hash<std::string_view>{}(sv);
  }
  [[nodiscard]] size_t operator()(const std::string& s) const {
    return std::hash<std::string>{}(s);
  }
};

struct StringEqual {
  using is_transparent = void;

  [[nodiscard]] bool operator()(std::string_view a, std::string_view b) const {
    return a == b;
  }
};

/// Parsed query string from an HTTP/FCGI request.
/// Supports multiple values for the same key (multi-valued parameters).
class QueryString {
public:
  /// Parse a raw query string (e.g. "a=1/2/3&t=5&a=4/5/6").
  explicit QueryString(std::string_view raw);

  /// Get the first value for a key, or std::nullopt if not present.
  [[nodiscard]] std::optional<std::string_view> get(std::string_view key) const;

  /// Get all values for a key.
  [[nodiscard]] std::vector<std::string_view> get_all(std::string_view key) const;

  /// Check if a key exists.
  [[nodiscard]] bool has(std::string_view key) const;

  /// Get the number of unique keys.
  [[nodiscard]] size_t size() const { return params_.size(); }

private:
  std::unordered_map<std::string, std::vector<std::string>, StringHash, StringEqual> params_;
};

}  // namespace cvknxd
