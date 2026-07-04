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

#include "knxd_client.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

#include "knxd_protocol.h"

namespace cvknxd {

struct KnxdClient::Impl {
  int fd = -1;
  GroupTelegramCallback telegram_callback;
  bool group_socket_open = false;
  std::vector<uint8_t> read_buffer_;  // buffered partial reads for non-blocking mode

  ~Impl() {
    if (fd >= 0) {
      ::close(fd);
    }
  }
};

KnxdClient::KnxdClient() : impl_(std::make_unique<Impl>()) {}

KnxdClient::~KnxdClient() = default;

KnxdClient::KnxdClient(KnxdClient&&) noexcept = default;
KnxdClient& KnxdClient::operator=(KnxdClient&&) noexcept = default;

bool KnxdClient::connect(std::string_view socket_path) {
  if (impl_->fd >= 0) {
    disconnect();
  }

  impl_->fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (impl_->fd < 0)
    return false;

  struct sockaddr_un addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  // Copy path safely
  if (socket_path.size() >= sizeof(addr.sun_path))
    return false;
  std::memcpy(addr.sun_path, socket_path.data(), socket_path.size());

  if (::connect(impl_->fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(impl_->fd);
    impl_->fd = -1;
    return false;
  }

  return true;
}

void KnxdClient::disconnect() {
  if (impl_->fd >= 0) {
    ::close(impl_->fd);
    impl_->fd = -1;
  }
  impl_->group_socket_open = false;
}

bool KnxdClient::is_connected() const {
  return impl_->fd >= 0;
}

namespace {

/// Write all bytes to socket. Returns true if all bytes were written.
bool write_all(int fd, const uint8_t* data, size_t len) {
  while (len > 0) {
    ssize_t written = ::write(fd, data, len);
    if (written < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    data += written;
    len -= static_cast<size_t>(written);
  }
  return true;
}

/// Read a complete eibd message (length-prefixed) from the socket.
/// Uses an internal buffer to handle partial reads in non-blocking mode.
/// @param fd Socket file descriptor.
/// @param buffer Accumulated read buffer (consumed as messages are parsed).
/// @return Complete message bytes (including 2-byte length header), or std::nullopt.
std::optional<std::vector<uint8_t>> read_message(int fd, std::vector<uint8_t>& buffer) {
  // Try to parse a complete message from the buffer first
  while (buffer.size() >= 2) {
    uint16_t payload_len = static_cast<uint16_t>((buffer[0] << 8) | buffer[1]);
    size_t total_needed = 2 + static_cast<size_t>(payload_len);

    if (buffer.size() >= total_needed) {
      // Complete message available — extract it
      std::vector<uint8_t> msg(buffer.begin(), buffer.begin() + static_cast<ptrdiff_t>(total_needed));
      buffer.erase(buffer.begin(), buffer.begin() + static_cast<ptrdiff_t>(total_needed));
      return msg;
    }
    // Need more data — will read below
    break;
  }

  // Read more data from socket into buffer
  uint8_t tmp[4096];
  ssize_t n = ::read(fd, tmp, sizeof(tmp));
  if (n > 0) {
    buffer.insert(buffer.end(), tmp, tmp + n);
    // Recurse to try parsing again with new data
    return read_message(fd, buffer);
  }
  if (n == 0) {
    return std::nullopt;  // EOF — connection closed
  }
  // n < 0
  if (errno == EAGAIN || errno == EWOULDBLOCK) {
    return std::nullopt;  // No data available in non-blocking mode
  }
  if (errno == EINTR) {
    return read_message(fd, buffer);  // Retry on signal
  }
  return std::nullopt;  // Real error
}

}  // namespace

bool KnxdClient::open_group_socket(bool write_only) {
  if (!is_connected())
    return false;

  uint8_t wo_byte = write_only ? 0xFF : 0x00;
  std::vector<uint8_t> data = {wo_byte};
  auto msg = build_eibd_message(EibMessageType::OPEN_GROUPCON, data);

  if (!write_all(impl_->fd, msg.data(), msg.size()))
    return false;

  // Read response
  auto resp = read_message(impl_->fd, impl_->read_buffer_);
  if (!resp)
    return false;

  uint16_t resp_type;
  std::vector<uint8_t> resp_data;
  if (!parse_eibd_message(*resp, resp_type, resp_data))
    return false;

  // Success: response is OPEN_GROUPCON with no error
  impl_->group_socket_open = (resp_type == EibMessageType::OPEN_GROUPCON);
  return impl_->group_socket_open;
}

bool KnxdClient::send_group_packet(uint16_t group_addr, const std::vector<uint8_t>& apdu) {
  if (!is_connected() || !impl_->group_socket_open)
    return false;

  std::vector<uint8_t> data;
  data.reserve(2 + apdu.size());
  data.push_back(static_cast<uint8_t>((group_addr >> 8) & 0xFF));
  data.push_back(static_cast<uint8_t>(group_addr & 0xFF));
  data.insert(data.end(), apdu.begin(), apdu.end());

  auto msg = build_eibd_message(EibMessageType::GROUP_PACKET, data);
  return write_all(impl_->fd, msg.data(), msg.size());
}

std::optional<std::vector<uint8_t>> KnxdClient::cache_read(uint16_t group_addr, bool nowait) {
  if (!is_connected())
    return std::nullopt;

  uint16_t msg_type = nowait ? EibMessageType::CACHE_READ_NOWAIT : EibMessageType::CACHE_READ;
  std::vector<uint8_t> data = {static_cast<uint8_t>((group_addr >> 8) & 0xFF),
                               static_cast<uint8_t>(group_addr & 0xFF)};

  auto msg = build_eibd_message(msg_type, data);
  if (!write_all(impl_->fd, msg.data(), msg.size()))
    return std::nullopt;

  auto resp = read_message(impl_->fd, impl_->read_buffer_);
  if (!resp)
    return std::nullopt;

  uint16_t resp_type;
  std::vector<uint8_t> resp_data;
  if (!parse_eibd_message(*resp, resp_type, resp_data))
    return std::nullopt;

  if (resp_type == msg_type && resp_data.size() >= 6) {
    // Response format: src(2) + dst(2) + apdu_data...
    return std::vector<uint8_t>(resp_data.begin() + 4, resp_data.end());
  }

  return std::nullopt;
}

int KnxdClient::get_fd() const {
  return impl_->fd;
}

void KnxdClient::set_nonblocking(bool enable) {
  if (impl_->fd < 0) return;
  int flags = ::fcntl(impl_->fd, F_GETFL, 0);
  if (flags < 0) return;
  if (enable) {
    ::fcntl(impl_->fd, F_SETFL, flags | O_NONBLOCK);
  } else {
    ::fcntl(impl_->fd, F_SETFL, flags & ~O_NONBLOCK);
  }
}

bool KnxdClient::poll_group_telegram(uint16_t& out_group_addr, std::vector<uint8_t>& out_apdu) {
  if (!is_connected())
    return false;

  // Try non-blocking read of message (uses internal buffer)
  auto msg = read_message(impl_->fd, impl_->read_buffer_);
  if (!msg)
    return false;

  uint16_t msg_type;
  std::vector<uint8_t> msg_data;
  if (!parse_eibd_message(*msg, msg_type, msg_data))
    return false;

  if (msg_type == EibMessageType::APDU_PACKET && msg_data.size() >= 4) {
    // Format: src_addr(2) + apdu...
    out_group_addr = static_cast<uint16_t>((msg_data[0] << 8) | msg_data[1]);
    out_apdu.assign(msg_data.begin() + 2, msg_data.end());

    // Invoke the telegram callback so cache and long-poll waiters are notified
    if (impl_->telegram_callback) {
      impl_->telegram_callback(out_group_addr, out_apdu);
    }

    return true;
  }

  return false;
}

void KnxdClient::set_telegram_callback(GroupTelegramCallback callback) {
  impl_->telegram_callback = std::move(callback);
}

}  // namespace cvknxd
