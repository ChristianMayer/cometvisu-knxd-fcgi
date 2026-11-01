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

/**
 * @file knxd_client.h
 * @brief Single-connection knxd Unix socket client.
 *
 * Opens one group socket to knxd for sending group telegrams
 * (EIB_GROUP_PACKET) and receiving APDU_PACKET telegrams from the
 * KNX bus.  No cache connection — GroupCache handles all data storage
 * and position tracking locally.
 *
 * The abstract KnxdClientInterface exists solely for test mocking
 * via MockKnxdClient.
 */

#ifndef COMETVISU_KNXD_FCGI_KNXD_CLIENT_H_
#define COMETVISU_KNXD_FCGI_KNXD_CLIENT_H_

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace cvknxd {

/// Abstract interface for knxd communication — enables MockKnxdClient in tests.
// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions)
class KnxdClientInterface {
public:
  KnxdClientInterface() = default;
  virtual ~KnxdClientInterface() = default;

  /// Connect to knxd via Unix domain socket.
  [[nodiscard]] virtual bool connect(std::string_view socket_path) = 0;

  /// Disconnect and close the socket.
  virtual void disconnect() = 0;

  /// Reconnect using the last-known socket path.
  [[nodiscard]] virtual bool reconnect() = 0;

  /// Whether the socket is currently connected.
  [[nodiscard]] virtual bool is_connected() const = 0;

  /// Open a group socket for sending/receiving group telegrams.
  /// @param write_only If true, the socket only sends (no incoming APDU_PACKET).
  [[nodiscard]] virtual bool open_group_socket(bool write_only) = 0;

  /// Send a group telegram (EIB_GROUP_PACKET) to the given address.
  /// @param group_addr 16-bit EIB group address.
  /// @param apdu Encoded APDU bytes including the 2-byte header.
  [[nodiscard]] virtual bool send_group_packet(uint16_t group_addr,
                                               const std::vector<uint8_t>& apdu) = 0;

  /// Try to receive one group telegram (APDU_PACKET) non-blocking.
  /// @return true if a telegram was consumed.
  [[nodiscard]] virtual bool poll_group_telegram(uint16_t& out_group_addr,
                                                 std::vector<uint8_t>& out_apdu) = 0;

  /// Underlying socket fd (for poll/select).  Returns -1 if not connected.
  [[nodiscard]] virtual int get_fd() const = 0;

  /// Total group telegrams received — used as the `i` position counter.
  [[nodiscard]] virtual uint64_t get_telegram_count() const = 0;

  /// Set non-blocking mode on the socket.
  virtual void set_nonblocking(bool enable) = 0;

  /// Result of wait_for_activity().
  enum class WaitResult : std::uint8_t {
    Timeout = 0,    ///< No data within the timeout — the connection is fine.
    GroupData = 1,  ///< Data available on the group socket.
    Error = 2       ///< Connection is gone (POLLHUP/POLLERR/POLLNVAL or not connected).
  };

  /// Block until data arrives on the group socket or timeout expires.
  /// Zero CPU — the kernel puts the process to sleep via poll().
  /// @param timeout_ms Maximum wait time in milliseconds.
  [[nodiscard]] virtual WaitResult wait_for_activity(int timeout_ms) = 0;
};

/// Real KnxdClient — POSIX Unix socket to knxd.
class KnxdClient : public KnxdClientInterface {
public:
  KnxdClient();
  ~KnxdClient() override;

  // Move-only (owns socket fd).
  KnxdClient(KnxdClient&&) noexcept;
  KnxdClient& operator=(KnxdClient&&) noexcept;
  KnxdClient(const KnxdClient&) = delete;
  KnxdClient& operator=(const KnxdClient&) = delete;

  [[nodiscard]] bool connect(std::string_view socket_path) override;
  void disconnect() override;
  [[nodiscard]] bool reconnect() override;
  [[nodiscard]] bool is_connected() const override;
  [[nodiscard]] bool open_group_socket(bool write_only) override;
  [[nodiscard]] bool send_group_packet(uint16_t group_addr,
                                       const std::vector<uint8_t>& apdu) override;
  [[nodiscard]] bool poll_group_telegram(uint16_t& out_group_addr,
                                         std::vector<uint8_t>& out_apdu) override;
  [[nodiscard]] int get_fd() const override;
  [[nodiscard]] uint64_t get_telegram_count() const override;
  void set_nonblocking(bool enable) override;

  /// @brief Maximum time to wait for the EIB_OPEN_GROUPCON reply.
  ///
  /// Without a deadline, a knxd that accepts the connection but never answers
  /// blocks the caller forever (this used to hang /w requests).
  void set_handshake_timeout_ms(int timeout_ms);

  [[nodiscard]] WaitResult wait_for_activity(int timeout_ms) override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cvknxd
#endif