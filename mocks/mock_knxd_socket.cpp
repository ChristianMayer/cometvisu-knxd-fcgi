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

bool MockKnxdClient::connect(std::string_view /*socket_path*/) {
  connected_ = connection_success_;
  return connected_;
}

void MockKnxdClient::disconnect() {
  connected_ = false;
  group_socket_open_ = false;
}

bool MockKnxdClient::is_connected() const {
  return connected_;
}

bool MockKnxdClient::open_group_socket(bool /*write_only*/) {
  if (!connected_)
    return false;
  group_socket_open_ = connection_success_;
  return group_socket_open_;
}

bool MockKnxdClient::send_group_packet(uint16_t group_addr, const std::vector<uint8_t>& apdu) {
  if (!connected_ || !group_socket_open_)
    return false;
  sent_packets_.push_back({group_addr, apdu});
  return true;
}

std::optional<std::vector<uint8_t>> MockKnxdClient::cache_read(uint16_t group_addr,
                                                               bool /*nowait*/) {
  if (!connected_)
    return std::nullopt;
  auto it = cached_values_.find(group_addr);
  if (it != cached_values_.end()) {
    return it->second;
  }
  return std::nullopt;
}

bool MockKnxdClient::poll_group_telegram(uint16_t& out_group_addr, std::vector<uint8_t>& out_apdu) {
  if (telegram_queue_.empty())
    return false;
  auto& front = telegram_queue_.front();
  out_group_addr = front.first;
  out_apdu = front.second;
  telegram_queue_.pop();
  return true;
}

int MockKnxdClient::get_fd() const {
  return -1;  // mock has no real fd
}

void MockKnxdClient::set_nonblocking(bool /*enable*/) {
  // no-op for mock
}

void MockKnxdClient::set_cached_value(uint16_t addr, const std::vector<uint8_t>& data) {
  cached_values_[addr] = data;
}

void MockKnxdClient::enqueue_telegram(uint16_t addr, const std::vector<uint8_t>& apdu) {
  telegram_queue_.push({addr, apdu});
}

void MockKnxdClient::reset() {
  connected_ = false;
  group_socket_open_ = false;
  connection_success_ = true;
  cached_values_.clear();
  while (!telegram_queue_.empty())
    telegram_queue_.pop();
  sent_packets_.clear();
}

}  // namespace cvknxd
