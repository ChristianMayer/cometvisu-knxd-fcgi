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

#ifndef COMETVISU_KNXD_FCGI_KNXD_CLIENT_H_
#define COMETVISU_KNXD_FCGI_KNXD_CLIENT_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "knxd_protocol.h"

namespace cvknxd {

/// Interface for knxd communication — allows mocking in tests.
// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions)
class KnxdClientInterface {
public:
  virtual ~KnxdClientInterface() = default;

  /// Connect to knxd socket. Returns true on success.
  [[nodiscard]] virtual bool connect(std::string_view socket_path) = 0;

  /// Disconnect from knxd.
  virtual void disconnect() = 0;

  /// Attempt to reconnect after a disconnect, using the last known socket path
  /// and group socket settings. Returns true if reconnection and group socket
  /// re-opening succeeded.
  [[nodiscard]] virtual bool reconnect() = 0;

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

  /// Query knxd for group addresses that changed since a given position.
  /// This is the COMET/long-poll primitive — it blocks until updates arrive
  /// or the timeout expires (like the original EIB_Cache_LastUpdates).
  /// Uses the EIB_CACHE_LAST_UPDATES_2 protocol message with 32-bit counters.
  /// @param start The starting position (only updates after this are returned).
  /// @param timeout_sec How long to wait for updates (seconds, 0 = return immediately).
  /// @return LastUpdatesResult with changed addresses and new position, or std::nullopt on error.
  [[nodiscard]] virtual std::optional<LastUpdatesResult> cache_last_updates_2(uint32_t start,
                                                                              int timeout_sec) = 0;

  /// Try to receive a group telegram (non-blocking).
  /// @param out_group_addr Output: source of the telegram.
  /// @param out_apdu Output: received APDU bytes.
  /// @return true if a telegram was received.
  [[nodiscard]] virtual bool poll_group_telegram(uint16_t& out_group_addr,
                                                 std::vector<uint8_t>& out_apdu) = 0;

  /// Get the cache file descriptor for poll()/select() integration.
  /// This is the fd of the separate cache connection — used for long-poll
  /// reads where we block on cache_last_updates_2 responses.
  /// Returns -1 if not connected.
  [[nodiscard]] virtual int get_cache_fd() const = 0;
  /// Returns -1 if not connected.
  ///
  /// WARNING: The returned fd is only valid while the calling thread holds
  /// the appropriate lock. After the lock is released, another thread may
  /// call disconnect() or reconnect(), invalidating the fd. The caller must
  /// not use the fd after releasing the lock.
  [[nodiscard]] virtual int get_fd() const = 0;

  /// Get the total number of group telegrams received from the knxd bus.
  /// This is used as the "i" (index/state-version) field in CometVisu responses.
  [[nodiscard]] virtual uint64_t get_telegram_count() const = 0;

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
  [[nodiscard]] bool reconnect() override;
  [[nodiscard]] bool is_connected() const override;
  [[nodiscard]] bool open_group_socket(bool write_only) override;
  [[nodiscard]] bool send_group_packet(uint16_t group_addr,
                                       const std::vector<uint8_t>& apdu) override;
  [[nodiscard]] std::optional<std::vector<uint8_t>> cache_read(uint16_t group_addr,
                                                               bool nowait) override;
  [[nodiscard]] std::optional<LastUpdatesResult> cache_last_updates_2(uint32_t start,
                                                                      int timeout_sec) override;
  [[nodiscard]] bool poll_group_telegram(uint16_t& out_group_addr,
                                         std::vector<uint8_t>& out_apdu) override;
  [[nodiscard]] int get_fd() const override;
  [[nodiscard]] int get_cache_fd() const override;
  [[nodiscard]] uint64_t get_telegram_count() const override;
  void set_nonblocking(bool enable) override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cvknxd

#endif  // COMETVISU_KNXD_FCGI_KNXD_CLIENT_H_
