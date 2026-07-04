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

#include "knxd_protocol.h"

#include <charconv>
#include <cstring>
#include <stdexcept>

namespace cvknxd {

// ---- KnxGroupAddress ----

std::optional<KnxGroupAddress> KnxGroupAddress::from_string(std::string_view str) {
  KnxGroupAddress addr;
  const char* p = str.data();
  const char* end = str.data() + str.size();

  auto parse_part = [&](uint8_t& out, int max_val) -> bool {
    if (p >= end || *p < '0' || *p > '9')
      return false;
    int val = 0;
    while (p < end && *p >= '0' && *p <= '9') {
      val = val * 10 + (*p - '0');
      if (val > max_val)
        return false;
      ++p;
    }
    out = static_cast<uint8_t>(val);
    return true;
  };

  if (!parse_part(addr.main, 31))
    return std::nullopt;
  if (p >= end || *p != '/')
    return std::nullopt;
  ++p;
  if (!parse_part(addr.middle, 7))
    return std::nullopt;
  if (p >= end || *p != '/')
    return std::nullopt;
  ++p;
  if (!parse_part(addr.sub, 255))
    return std::nullopt;
  if (p != end)
    return std::nullopt;  // trailing garbage

  return addr;
}

std::string KnxGroupAddress::to_string() const {
  return std::to_string(main) + "/" + std::to_string(middle) + "/" + std::to_string(sub);
}

uint16_t KnxGroupAddress::to_eibaddr() const {
  return static_cast<uint16_t>((main << 11) | (middle << 8) | sub);
}

KnxGroupAddress KnxGroupAddress::from_eibaddr(uint16_t addr) {
  return KnxGroupAddress{static_cast<uint8_t>((addr >> 11) & 0x1F),
                         static_cast<uint8_t>((addr >> 8) & 0x07),
                         static_cast<uint8_t>(addr & 0xFF)};
}

// ---- KnxAddress ----

std::optional<KnxAddress> KnxAddress::from_cometvisu(std::string_view str) {
  auto colon = str.find(':');
  if (colon == std::string_view::npos) {
    // No namespace: assume "KNX" as default
    auto group = KnxGroupAddress::from_string(str);
    if (!group)
      return std::nullopt;
    return KnxAddress{"KNX", *group};
  }

  KnxAddress result;
  result.ns = std::string{str.substr(0, colon)};
  auto group = KnxGroupAddress::from_string(str.substr(colon + 1));
  if (!group)
    return std::nullopt;
  result.group = *group;
  return result;
}

std::string KnxAddress::to_cometvisu() const {
  return ns + ":" + group.to_string();
}

// ---- APDU ----

std::vector<uint8_t> build_apdu(ApduType type, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> result;
  result.reserve(2 + data.size());
  result.push_back(0x00);  // APDU byte 0 for group value

  if (data.empty()) {
    // Read request: just the type marker
    result.push_back(static_cast<uint8_t>(type));
  } else if (data.size() == 1) {
    // Single-byte value: pack into APDU byte 1
    result.push_back(static_cast<uint8_t>(type) | (data[0] & 0x3F));
  } else {
    // Multi-byte value: type marker in byte 1, data follows
    result.push_back(static_cast<uint8_t>(type));
    result.insert(result.end(), data.begin(), data.end());
  }

  return result;
}

bool parse_apdu(const std::vector<uint8_t>& apdu, ApduType& out_type,
                std::vector<uint8_t>& out_data) {
  if (apdu.size() < 2)
    return false;

  out_type = static_cast<ApduType>(apdu[1] & 0xC0);
  out_data.clear();

  switch (out_type) {
    case ApduType::Read:
      // Read has no data
      break;
    case ApduType::Response:
    case ApduType::Write:
      if (apdu.size() == 2) {
        // Single byte value packed in lower 6 bits of byte 1.
        // Note: This limits single-byte values to 0x00–0x3F (6 bits).
        // For values > 0x3F, use multi-byte format with data in bytes 2+.
        out_data.push_back(apdu[1] & 0x3F);
      } else {
        // Multi-byte value: type marker is in byte 1,
        // all data bytes follow starting at byte 2.
        out_data.assign(apdu.begin() + 2, apdu.end());
      }
      break;
  }

  return true;
}

// ---- EIBD Messages ----

std::vector<uint8_t> build_eibd_message(uint16_t type, const std::vector<uint8_t>& data) {
  // Payload: [type_hi, type_lo] + [data...]
  size_t payload_size = 2 + data.size();
  std::vector<uint8_t> result;
  result.reserve(2 + payload_size);

  // Length header (big-endian)
  result.push_back(static_cast<uint8_t>((payload_size >> 8) & 0xFF));
  result.push_back(static_cast<uint8_t>(payload_size & 0xFF));

  // Type
  result.push_back(static_cast<uint8_t>((type >> 8) & 0xFF));
  result.push_back(static_cast<uint8_t>(type & 0xFF));

  // Data
  result.insert(result.end(), data.begin(), data.end());

  return result;
}

bool parse_eibd_message(const std::vector<uint8_t>& raw, uint16_t& out_type,
                        std::vector<uint8_t>& out_data) {
  if (raw.size() < 4)
    return false;  // need length(2) + type(2) minimum

  // Verify length
  uint16_t payload_len = static_cast<uint16_t>((raw[0] << 8) | raw[1]);
  if (raw.size() < static_cast<size_t>(payload_len) + 2)
    return false;

  out_type = static_cast<uint16_t>((raw[2] << 8) | raw[3]);
  out_data.assign(raw.begin() + 4, raw.begin() + 2 + payload_len);

  return true;
}

}  // namespace cvknxd
