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

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cvknxd {

/// Interface for knxd communication — allows mocking in tests.
class KnxdClientInterface {
public:
  virtual ~KnxdClientInterface() = default;

  /// Connect to knxd socket. Returns true on success.
  [[nodiscard]] virtual bool connect(std::string_view socket_path) = 0;

  /// Disconnect from knxd.
  virtual void disconnect() = 0;

  /// Check if connected.
  [[nodiscard]] virtual bool is_connected() const = 0;

  /// Open a group socket for listening to group telegrams.
  /// @param write_only If true, only send, don't receive.
  /// @return true on success.
  [[nodiscard]] virtual bool open_group_socket(bool write_only) = 0;

  /// Send a group telegram (write or read request).
  /// @param group_addr 16-bit EIB group address.
  /// @param apdu Encoded APDU bytes (including 2-byte APDU header).
  /// @return true on success.
  [[nodiscard]] virtual bool send_group_packet(uint16_t group_addr,
                                               const std::vector<uint8_t>& apdu) = 0;

  /// Read a group value from the knxd cache.
  /// @param group_addr 16-bit EIB group address.
  /// @param nowait If true, return immediately even if no value cached.
  /// @return Cached value bytes (APDU data after header), or std::nullopt.
  [[nodiscard]] virtual std::optional<std::vector<uint8_t>> cache_read(uint16_t group_addr,
                                                                       bool nowait) = 0;

  /// Try to receive a group telegram (non-blocking).
  /// @param out_group_addr Output: source of the telegram.
  /// @param out_apdu Output: received APDU bytes.
  /// @return true if a telegram was received.
  [[nodiscard]] virtual bool poll_group_telegram(uint16_t& out_group_addr,
                                                 std::vector<uint8_t>& out_apdu) = 0;

  /// Get the underlying file descriptor for poll()/select() integration.
  /// Returns -1 if not connected.
  [[nodiscard]] virtual int get_fd() const = 0;

  /// Set the socket to non-blocking mode.
  virtual void set_nonblocking(bool enable) = 0;
};

/// Real implementation of KnxdClientInterface using Unix sockets.
class KnxdClient : public KnxdClientInterface {
public:
  KnxdClient();
  ~KnxdClient() override;

  // Move-only
  KnxdClient(KnxdClient&&) noexcept;
  KnxdClient& operator=(KnxdClient&&) noexcept;
  KnxdClient(const KnxdClient&) = delete;
  KnxdClient& operator=(const KnxdClient&) = delete;

  [[nodiscard]] bool connect(std::string_view socket_path) override;
  void disconnect() override;
  [[nodiscard]] bool is_connected() const override;
  [[nodiscard]] bool open_group_socket(bool write_only) override;
  [[nodiscard]] bool send_group_packet(uint16_t group_addr,
                                       const std::vector<uint8_t>& apdu) override;
  [[nodiscard]] std::optional<std::vector<uint8_t>> cache_read(uint16_t group_addr,
                                                               bool nowait) override;
  [[nodiscard]] bool poll_group_telegram(uint16_t& out_group_addr,
                                         std::vector<uint8_t>& out_apdu) override;
  [[nodiscard]] int get_fd() const override;
  void set_nonblocking(bool enable) override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cvknxd
