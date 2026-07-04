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

#include "query_string.h"

#include <algorithm>

namespace cvknxd {

namespace {

// Simple URL percent-decoder
std::string url_decode(std::string_view str) {
  std::string result;
  result.reserve(str.size());
  for (size_t i = 0; i < str.size(); ++i) {
    if (str[i] == '%' && i + 2 < str.size()) {
      auto from_hex = [](char c) -> int {
        if (c >= '0' && c <= '9')
          return c - '0';
        if (c >= 'a' && c <= 'f')
          return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
          return c - 'A' + 10;
        return -1;
      };
      int hi = from_hex(str[i + 1]);
      int lo = from_hex(str[i + 2]);
      if (hi >= 0 && lo >= 0) {
        result.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    if (str[i] == '+') {
      result.push_back(' ');
    } else {
      result.push_back(str[i]);
    }
  }
  return result;
}

}  // namespace

QueryString::QueryString(std::string_view raw) {
  if (raw.empty())
    return;

  size_t pos = 0;
  while (pos < raw.size()) {
    // Find key=value pair
    size_t eq = raw.find('=', pos);
    size_t amp = raw.find('&', pos);

    if (eq == std::string_view::npos || (amp != std::string_view::npos && eq > amp)) {
      // Malformed: no '=' before next '&', skip this pair
      pos = (amp == std::string_view::npos) ? raw.size() : amp + 1;
      continue;
    }

    std::string key{url_decode(raw.substr(pos, eq - pos))};
    size_t val_end = (amp == std::string_view::npos) ? raw.size() : amp;
    std::string value{url_decode(raw.substr(eq + 1, val_end - eq - 1))};

    params_[key].push_back(std::move(value));
    pos = (amp == std::string_view::npos) ? raw.size() : amp + 1;
  }
}

std::optional<std::string_view> QueryString::get(std::string_view key) const {
  auto it = params_.find(key);
  if (it != params_.end() && !it->second.empty()) {
    return std::string_view{it->second.front()};
  }
  return std::nullopt;
}

std::vector<std::string_view> QueryString::get_all(std::string_view key) const {
  auto it = params_.find(key);
  if (it != params_.end()) {
    std::vector<std::string_view> result;
    result.reserve(it->second.size());
    for (const auto& v : it->second) {
      result.push_back(std::string_view{v});
    }
    return result;
  }
  return {};
}

bool QueryString::has(std::string_view key) const {
  return params_.find(key) != params_.end();
}

}  // namespace cvknxd
