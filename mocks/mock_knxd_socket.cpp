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

#include "mock_knxd_socket.h"

namespace cvknxd {

bool MockKnxdClient::connect(std::string_view socket_path) {
  last_socket_path_ = socket_path;
  if (connect_fail_count_ > 0) {
    connect_fail_count_--;
    connected_ = false;
    return false;
  }
  connected_ = connection_success_;
  return connected_;
}

void MockKnxdClient::disconnect() {
  connected_ = false;
  group_socket_open_ = false;
}

bool MockKnxdClient::reconnect() {
  if (last_socket_path_.empty()) {
    return false;  // never connected
  }
  connected_ = connection_success_;
  group_socket_open_ = false;  // caller must re-open group socket
  return connected_;
}

bool MockKnxdClient::is_connected() const {
  return connected_;
}

bool MockKnxdClient::open_group_socket(bool /*write_only*/) {
  if (!connected_) {
    return false;
  }
  group_socket_open_ = connection_success_;
  return group_socket_open_;
}

bool MockKnxdClient::send_group_packet(uint16_t group_addr, const std::vector<uint8_t>& apdu) {
  if (!connected_ || !group_socket_open_) {
    return false;
  }
  if (send_fail_count_ > 0) {
    send_fail_count_--;
    return false;
  }
  sent_packets_.push_back({.group_addr = group_addr, .apdu = apdu});
  return true;
}

std::optional<std::vector<uint8_t>> MockKnxdClient::cache_read(uint16_t group_addr,
                                                               bool /*nowait*/) {
  if (!connected_) {
    return std::nullopt;
  }
  if (cache_read_fail_count_ > 0) {
    cache_read_fail_count_--;
    connected_ = false;
    return std::nullopt;
  }
  auto it = cached_values_.find(group_addr);
  if (it != cached_values_.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::optional<LastUpdatesResult> MockKnxdClient::cache_last_updates_2(uint32_t start,
                                                                      int /*timeout_sec*/) {
  cache_last_updates_call_count_++;

  if (!connected_) {
    return std::nullopt;
  }

  // Simulate connection failures for testing reconnection resilience.
  // Set connected_=false so the handler can detect the failure via is_connected()
  // and trigger a reconnect().
  if (cache_updates_fail_count_ > 0) {
    cache_updates_fail_count_--;
    connected_ = false;
    return std::nullopt;
  }

  // Return the first queued result that matches the start position,
  // or the first result if no specific matching is configured.
  if (last_updates_queue_.empty()) {
    return std::nullopt;
  }

  auto state = last_updates_queue_.front();
  last_updates_queue_.pop();

  // If the test specified an after_position, only return if start matches
  if (state.after_position != 0 && state.after_position != start) {
    // Re-queue and return empty (no updates yet)
    last_updates_queue_.push(state);
    LastUpdatesResult empty;
    empty.new_position = start;  // unchanged
    return empty;
  }

  LastUpdatesResult result;
  result.changed_addresses = state.changed_addrs;
  result.new_position = state.new_position;
  return result;
}

bool MockKnxdClient::poll_group_telegram(uint16_t& out_group_addr, std::vector<uint8_t>& out_apdu) {
  if (telegram_queue_.empty()) {
    return false;
  }
  auto& front = telegram_queue_.front();
  out_group_addr = front.first;
  out_apdu = front.second;
  telegram_queue_.pop();
  telegram_count_++;
  return true;
}

int MockKnxdClient::get_fd() const {
  return -1;  // mock has no real fd
}

uint64_t MockKnxdClient::get_telegram_count() const {
  return telegram_count_;
}

void MockKnxdClient::set_nonblocking(bool /*enable*/) {
  // no-op for mock
}

void MockKnxdClient::set_cached_value(uint16_t addr, const std::vector<uint8_t>& data) {
  cached_values_[addr] = data;
}

void MockKnxdClient::enqueue_telegram(uint16_t addr, const std::vector<uint8_t>& apdu) {
  telegram_queue_.emplace(addr, apdu);
}

void MockKnxdClient::reset() {
  connected_ = false;
  group_socket_open_ = false;
  connection_success_ = true;
  last_socket_path_.clear();
  cached_values_.clear();
  while (!telegram_queue_.empty()) {
    telegram_queue_.pop();
  }
  sent_packets_.clear();
  telegram_count_ = 0;
  while (!last_updates_queue_.empty()) {
    last_updates_queue_.pop();
  }
  cache_updates_fail_count_ = 0;
  cache_read_fail_count_ = 0;
  cache_last_updates_call_count_ = 0;
}

void MockKnxdClient::set_last_updates_result(uint32_t after_position,
                                             const std::vector<uint16_t>& changed_addrs,
                                             uint32_t new_position) {
  last_updates_queue_.push({.after_position = after_position,
                            .changed_addrs = changed_addrs,
                            .new_position = new_position});
}

}  // namespace cvknxd
