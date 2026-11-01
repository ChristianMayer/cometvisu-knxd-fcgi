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
#include <vector>

#include "../src/knxd/knxd_client.h"

namespace cvknxd {

class MockKnxdClient : public KnxdClientInterface {
public:
  MockKnxdClient() = default;

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
  [[nodiscard]] WaitResult wait_for_activity(int timeout_ms) override;

  // --- test helpers ---
  void set_connection_success(bool success) { connection_success_ = success; }
  void enqueue_telegram(uint16_t addr, const std::vector<uint8_t>& apdu);
  struct SentPacket { uint16_t group_addr; std::vector<uint8_t> apdu; };
  [[nodiscard]] std::vector<SentPacket> sent_packets() const { return sent_packets_; }
  void reset();
  void set_telegram_count(uint64_t count) { telegram_count_ = count; }
  void set_send_fail_count(int count) { send_fail_count_ = count; }
  void set_connect_fail_count(int count) { connect_fail_count_ = count; }
  /// Number of upcoming wait_for_activity() calls that report a lost connection.
  void set_wait_error_count(int count) { wait_error_count_ = count; }
  /// Number of connect() calls so far (used to detect reconnect behaviour).
  [[nodiscard]] int connect_calls() const { return connect_calls_; }
  /// Number of wait_for_activity() calls so far (used to detect busy loops).
  [[nodiscard]] int wait_calls() const { return wait_calls_; }

private:
  bool connected_ = false;
  bool group_socket_open_ = false;
  bool connection_success_ = true;
  int connect_fail_count_ = 0;
  int wait_error_count_ = 0;
  int connect_calls_ = 0;
  int wait_calls_ = 0;
  std::string last_socket_path_;
  std::queue<std::pair<uint16_t, std::vector<uint8_t>>> telegram_queue_;
  std::vector<SentPacket> sent_packets_;
  uint64_t telegram_count_ = 0;
  int send_fail_count_ = 0;
};

}  // namespace cvknxd