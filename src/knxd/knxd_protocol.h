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

#ifndef COMETVISU_KNXD_FCGI_KNXD_PROTOCOL_H_
#define COMETVISU_KNXD_FCGI_KNXD_PROTOCOL_H_

#include <eibtypes.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cvknxd {

/// KNX group address: three-level X/Y/Z representation and 16-bit internal form.
struct KnxGroupAddress {
  uint8_t main = 0;    // X in X/Y/Z
  uint8_t middle = 0;  // Y in X/Y/Z
  uint8_t sub = 0;     // Z in X/Y/Z

  /// Create from three-level address string "X/Y/Z".
  /// Returns std::nullopt if the format is invalid.
  [[nodiscard]] static std::optional<KnxGroupAddress> from_string(std::string_view str);

  /// Convert to three-level string "X/Y/Z".
  [[nodiscard]] std::string to_string() const;

  /// Convert to 16-bit EIB group address.
  [[nodiscard]] uint16_t to_eibaddr() const;

  /// Create from 16-bit EIB group address.
  [[nodiscard]] static KnxGroupAddress from_eibaddr(uint16_t addr);

  bool operator==(const KnxGroupAddress&) const = default;
  bool operator!=(const KnxGroupAddress& other) const { return !(*this == other); }
};

/// KNX address namespace prefix and group address.
/// CometVisu format: "NAMESPACE:ADDRESS", e.g. "KNX:1/2/3".
///
/// When no namespace prefix is present in the input (no colon), the configured
/// default namespace is used. The default namespace is empty by default,
/// meaning addresses are just "X/Y/Z" with no prefix. Set via
/// set_default_namespace() or the ADDRESS_PREFIX environment variable.
struct KnxAddress {
  std::string ns;         // namespace, e.g. "KNX"
  KnxGroupAddress group;  // group address part

  /// Parse a CometVisu address string.
  /// If the string contains a colon, the part before the colon is the namespace.
  /// If there is no colon, the configured default namespace is used.
  /// Returns std::nullopt if the format is invalid.
  [[nodiscard]] static std::optional<KnxAddress> from_cometvisu(std::string_view str);

  /// Convert back to CometVisu address format.
  /// If the namespace is empty, produces just "X/Y/Z" (no prefix).
  /// If the namespace is non-empty, produces "NAMESPACE:X/Y/Z".
  [[nodiscard]] std::string to_cometvisu() const;

  /// Set the default namespace used when parsing addresses without a colon.
  static void set_default_namespace(std::string_view ns);

  /// Get the current default namespace.
  [[nodiscard]] static std::string_view get_default_namespace();

  bool operator==(const KnxAddress&) const = default;

private:
  static std::string default_namespace_;
};

/// APDU types for group communication.
enum class ApduType : uint8_t {
  Read = 0x00,      // A_GroupValue_Read
  Response = 0x40,  // A_GroupValue_Response
  Write = 0x80,     // A_GroupValue_Write
};

/// Build an APDU (Application Protocol Data Unit) for a group value.
/// @param type The APCI type (Read/Response/Write).
/// @param data The payload bytes.
/// @return Encoded APDU bytes ready for transmission.
[[nodiscard]] std::vector<uint8_t> build_apdu(ApduType type, const std::vector<uint8_t>& data);

/// Parse a received APDU.
/// @param apdu Raw APDU bytes (starting with the 2-byte APDU header).
/// @param out_type Output: the APCI type.
/// @param out_data Output: the payload bytes.
/// @return true if parsing succeeded.
[[nodiscard]] bool parse_apdu(const std::vector<uint8_t>& apdu, ApduType& out_type,
                              std::vector<uint8_t>& out_data);

/// EIB message type constants — from knxd's eibtypes.h.
/// Use the knxd-defined constants directly (EIB_OPEN_GROUPCON etc.) rather
/// than hard-coding them.
namespace EibMessageType {
inline constexpr uint16_t OPEN_GROUPCON = EIB_OPEN_GROUPCON;
inline constexpr uint16_t GROUP_PACKET = EIB_GROUP_PACKET;
inline constexpr uint16_t APDU_PACKET = EIB_APDU_PACKET;
inline constexpr uint16_t OPEN_T_GROUP = EIB_OPEN_T_GROUP;
inline constexpr uint16_t CACHE_READ = EIB_CACHE_READ;
inline constexpr uint16_t CACHE_READ_NOWAIT = EIB_CACHE_READ_NOWAIT;
inline constexpr uint16_t CACHE_LAST_UPDATES = EIB_CACHE_LAST_UPDATES;
inline constexpr uint16_t CACHE_LAST_UPDATES_2 = EIB_CACHE_LAST_UPDATES_2;
}  // namespace EibMessageType

/// Build a complete eibd wire message.
/// Format: [2 bytes length, big-endian] [payload]
/// @param type Message type (2 bytes).
/// @param data Additional payload data after the type.
/// @return Full wire-format message.
[[nodiscard]] std::vector<uint8_t> build_eibd_message(uint16_t type,
                                                      const std::vector<uint8_t>& data);

/// Build an EIB_OPEN_GROUPCON wire message.
/// knxd >= 0.14 expects a 5-byte payload: [type:2][reserved:2][write_only:1].
/// @param write_only If true, no group telegrams will be delivered.
/// @return Complete wire-format message ready to send.
[[nodiscard]] std::vector<uint8_t> build_open_groupcon(bool write_only);

/// Build an EIB_GROUP_PACKET wire message.
/// Payload: [type:2][dest_addr:2][apdu:N].
/// @param group_addr 16-bit EIB group destination address.
/// @param apdu Encoded APDU bytes (including 2-byte header).
/// @return Complete wire-format message ready to send.
[[nodiscard]] std::vector<uint8_t> build_group_packet(uint16_t group_addr,
                                                      const std::vector<uint8_t>& apdu);

/// Build an EIB_CACHE_READ wire message.
/// Payload: [type:2][addr:2].
/// @param group_addr 16-bit EIB group address to read from cache.
/// @return Complete wire-format message ready to send.
[[nodiscard]] std::vector<uint8_t> build_cache_read(uint16_t group_addr);

/// Build an EIB_CACHE_READ_NOWAIT wire message.
/// Payload: [type:2][addr:2].
/// @param group_addr 16-bit EIB group address to read from cache (non-blocking).
/// @return Complete wire-format message ready to send.
[[nodiscard]] std::vector<uint8_t> build_cache_read_nowait(uint16_t group_addr);

/// Build an EIB_CACHE_LAST_UPDATES_2 wire message.
/// Payload: [type:2][start:4][timeout:2].
/// Uses 32-bit position counters (unlike v1 which uses 16-bit).
/// @param start The starting position (only updates after this are returned).
/// @param timeout_sec How long to wait for updates (seconds, 0 = return immediately).
/// @return Complete wire-format message ready to send.
[[nodiscard]] std::vector<uint8_t> build_cache_last_updates_2(uint32_t start, int timeout_sec);

/// Result of a CACHE_LAST_UPDATES_2 response.
struct LastUpdatesResult {
  /// Group addresses that changed since the start position.
  std::vector<uint16_t> changed_addresses;
  /// New end position (pass this as "start" in the next call).
  uint32_t new_position = 0;
};

/// Parse an EIB_CACHE_LAST_UPDATES_2 response.
/// Response payload: [end:4][addrs:N*2]
/// @param data Raw payload data (after the 2-byte type).
/// @return Parsed result, or std::nullopt if the data is malformed.
[[nodiscard]] std::optional<LastUpdatesResult> parse_cache_last_updates_2_response(
    const std::vector<uint8_t>& data);

/// Parse a received eibd wire message.
/// @param raw Raw bytes received from socket.
/// @param out_type Output: extracted message type.
/// @param out_data Output: payload after type bytes.
/// @return true if parsing succeeded.
[[nodiscard]] bool parse_eibd_message(const std::vector<uint8_t>& raw, uint16_t& out_type,
                                      std::vector<uint8_t>& out_data);

/// Maximum size of the internal read buffer in bytes (1 MB).
/// Prevents unbounded memory growth from unconsumed telegrams.
inline constexpr size_t kMaxReadBufferSize = 1048576;

/// Try to extract a complete eibd message from an accumulated read buffer.
/// If a complete message is found (based on the 2-byte length prefix),
/// it is removed from the buffer and returned.
/// @param buffer The accumulated read buffer (modified in place).
/// @return Complete message bytes (including 2-byte length header), or std::nullopt
///         if no complete message is available yet.
[[nodiscard]] std::optional<std::vector<uint8_t>> try_extract_message(std::vector<uint8_t>& buffer);

}  // namespace cvknxd

#endif  // COMETVISU_KNXD_FCGI_KNXD_PROTOCOL_H_
