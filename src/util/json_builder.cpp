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

#include "json_builder.h"

namespace cvknxd {

void JsonBuilder::start_object() {
  buffer_.push_back('{');
  first_ = true;
  ++open_objects_;
}

void JsonBuilder::end_object() {
  buffer_.push_back('}');
  --open_objects_;
  first_ = false;
}

void JsonBuilder::add_comma() {
  if (!first_) {
    buffer_.push_back(',');
  }
}

void JsonBuilder::add_quoted(std::string_view str) {
  buffer_.push_back('"');
  for (char c : str) {
    switch (c) {
      case '"':
        buffer_.push_back('\\');
        buffer_.push_back('"');
        break;
      case '\\':
        buffer_.push_back('\\');
        buffer_.push_back('\\');
        break;
      case '\n':
        buffer_.push_back('\\');
        buffer_.push_back('n');
        break;
      case '\r':
        buffer_.push_back('\\');
        buffer_.push_back('r');
        break;
      case '\t':
        buffer_.push_back('\\');
        buffer_.push_back('t');
        break;
      default:
        buffer_.push_back(c);
        break;
    }
  }
  buffer_.push_back('"');
}

void JsonBuilder::add_string(std::string_view key, std::string_view value) {
  add_comma();
  add_quoted(key);
  buffer_.push_back(':');
  add_quoted(value);
  first_ = false;
}

void JsonBuilder::add_key(std::string_view key) {
  add_comma();
  add_quoted(key);
  buffer_.push_back(':');
  first_ = true;  // next value is first in nested object
}

void JsonBuilder::add_raw(std::string_view raw) {
  add_comma();
  buffer_.append(raw);
  first_ = false;
}

}  // namespace cvknxd
