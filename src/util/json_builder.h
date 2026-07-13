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

#include <string>
#include <string_view>

namespace cvknxd {

/// Minimal JSON builder for the simple JSON responses used by the CometVisu protocol.
/// No external dependency needed — the protocol only requires flat objects.
class JsonBuilder {
public:
  JsonBuilder() = default;

  /// Start a JSON object: "{"
  void start_object();

  /// End a JSON object: "}"
  void end_object();

  /// Add a string key-value pair: "key":"value"
  void add_string(std::string_view key, std::string_view value);

  /// Add an integer key-value pair: "key":<value> (unquoted, minimal JSON)
  void add_number(std::string_view key, int64_t value);

  /// Add a nested object as a value.
  /// Caller is responsible for calling start/end_object before/after.
  void add_key(std::string_view key);

  /// Append raw JSON (for already-built sub-objects).
  void add_raw(std::string_view raw);

  /// Get the built JSON string.
  [[nodiscard]] const std::string& str() const { return buffer_; }

  /// Move the built JSON string out.
  [[nodiscard]] std::string take() { return std::move(buffer_); }

  /// Clear for reuse.
  void clear() {
    buffer_.clear();
    first_ = true;
    open_objects_ = 0;
  }

private:
  std::string buffer_;
  bool first_ = true;
  int open_objects_ = 0;

  void add_comma();
  void add_quoted(std::string_view str);
};

}  // namespace cvknxd
