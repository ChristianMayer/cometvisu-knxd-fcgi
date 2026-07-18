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
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "../src/knxd/knxd_client.h"

namespace cvknxd {

/// Mock implementation of KnxdClientInterface for testing.
/// Pre-programmed with responses and records sent messages.
class MockKnxdClient : public KnxdClientInterface {
public:
  MockKnxdClient() = default;

  // ---- KnxdClientInterface implementation ----

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
  [[nodiscard]] int get_cache_fd() const override { return -1; }
  [[nodiscard]] uint64_t get_telegram_count() const override;
  void set_nonblocking(bool enable) override;

  // ---- Test helpers ----

  /// Set whether connect/open_group_socket succeed.
  void set_connection_success(bool success) { connection_success_ = success; }

  /// Set cached data to return for a specific group address.
  void set_cached_value(uint16_t addr, const std::vector<uint8_t>& data);

  /// Enqueue a telegram to be returned by poll_group_telegram.
  void enqueue_telegram(uint16_t addr, const std::vector<uint8_t>& apdu);

  /// Get the last sent group packet.
  struct SentPacket {
    uint16_t group_addr;
    std::vector<uint8_t> apdu;
  };
  [[nodiscard]] std::vector<SentPacket> sent_packets() const { return sent_packets_; }

  /// Clear all recorded state.
  void reset();

  /// Manually set the telegram count (for testing i= telemetry).
  void set_telegram_count(uint64_t count) { telegram_count_ = count; }

  /// Set up the result for the next cache_last_updates_2 call.
  /// The mock will return these changed addresses and new position.
  void set_last_updates_result(uint32_t after_position, const std::vector<uint16_t>& changed_addrs,
                               uint32_t new_position);

  /// Make the next N calls to cache_last_updates_2 return std::nullopt
  /// (simulating connection loss / knxd restart).
  void set_cache_last_updates_fail_count(int count) { cache_updates_fail_count_ = count; }

  /// Make the next N calls to cache_read return std::nullopt
  /// (simulating connection loss / knxd restart).
  void set_cache_read_fail_count(int count) { cache_read_fail_count_ = count; }

  /// Make the next N calls to connect() return false before succeeding.
  /// After N failures, connect() returns connection_success_.
  void set_connect_fail_count(int count) { connect_fail_count_ = count; }

  /// Get the number of times cache_last_updates_2 was called since last reset.
  [[nodiscard]] int cache_last_updates_call_count() const { return cache_last_updates_call_count_; }

private:
  bool connected_ = false;
  bool group_socket_open_ = false;
  bool connection_success_ = true;
  int connect_fail_count_ = 0;
  std::string last_socket_path_;
  std::unordered_map<uint16_t, std::vector<uint8_t>> cached_values_;
  std::queue<std::pair<uint16_t, std::vector<uint8_t>>> telegram_queue_;
  std::vector<SentPacket> sent_packets_;
  uint64_t telegram_count_ = 0;

  // For cache_last_updates_2 mock
  struct LastUpdatesState {
    uint32_t after_position;
    std::vector<uint16_t> changed_addrs;
    uint32_t new_position;
  };
  std::queue<LastUpdatesState> last_updates_queue_;

  // Fail-count controls for testing reconnection resilience
  int cache_updates_fail_count_ = 0;
  int cache_read_fail_count_ = 0;

  // Call counter for verifying retry behavior
  int cache_last_updates_call_count_ = 0;
};

}  // namespace cvknxd
