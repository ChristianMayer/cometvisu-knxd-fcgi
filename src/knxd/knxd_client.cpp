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

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#include "../util/debug_log.h"
#include "../util/hex.h"
#include "knxd_protocol.h"

namespace cvknxd {

struct KnxdClient::Impl {
  int fd = -1;
  bool group_socket_open = false;
  std::string socket_path_;           // stored for reconnect
  bool write_only_ = false;           // stored for reconnect
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

  // Use non-blocking connect with timeout to avoid hanging indefinitely
  int flags = ::fcntl(impl_->fd, F_GETFL, 0);
  if (flags >= 0) {
    ::fcntl(impl_->fd, F_SETFL, flags | O_NONBLOCK);
  }

  int ret = ::connect(impl_->fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
  if (ret < 0 && errno != EINPROGRESS) {
    ::close(impl_->fd);
    impl_->fd = -1;
    return false;
  }

  if (ret < 0) {
    // Connection in progress — wait with timeout
    struct pollfd pfd;
    pfd.fd = impl_->fd;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    int poll_ret = ::poll(&pfd, 1, 5000);  // 5 second timeout
    if (poll_ret <= 0) {
      ::close(impl_->fd);
      impl_->fd = -1;
      return false;
    }
    // Check if connection succeeded
    int so_error = 0;
    socklen_t len = sizeof(so_error);
    if (::getsockopt(impl_->fd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 || so_error != 0) {
      ::close(impl_->fd);
      impl_->fd = -1;
      return false;
    }
  }

  // Restore blocking mode (caller can set non-blocking via set_nonblocking())
  if (flags >= 0) {
    ::fcntl(impl_->fd, F_SETFL, flags & ~O_NONBLOCK);
  }

  // Store path for potential reconnect
  impl_->socket_path_ = socket_path;

  return true;
}

void KnxdClient::disconnect() {
  if (impl_->fd >= 0) {
    ::close(impl_->fd);
    impl_->fd = -1;
  }
  impl_->group_socket_open = false;
  impl_->read_buffer_.clear();
  // Note: socket_path_ and write_only_ are preserved for reconnect
}

bool KnxdClient::is_connected() const {
  return impl_->fd >= 0;
}

bool KnxdClient::reconnect() {
  if (impl_->socket_path_.empty())
    return false;  // never connected, nothing to reconnect to

  // Disconnect any stale state first
  if (impl_->fd >= 0) {
    ::close(impl_->fd);
    impl_->fd = -1;
  }
  impl_->group_socket_open = false;
  impl_->read_buffer_.clear();

  // Re-establish connection
  if (!connect(impl_->socket_path_))
    return false;

  // Re-open group socket with previous write_only setting
  if (!open_group_socket(impl_->write_only_)) {
    disconnect();
    return false;
  }

  // Restore non-blocking mode (the default for this application)
  set_nonblocking(true);

  return true;
}

namespace {

/// Write all bytes to socket. Handles EAGAIN in non-blocking mode by polling.
/// Returns true if all bytes were written.
bool write_all(int fd, const uint8_t* data, size_t len) {
  while (len > 0) {
    ssize_t written = ::write(fd, data, len);
    if (written < 0) {
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // Socket buffer full in non-blocking mode — wait for writability
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        int ret = ::poll(&pfd, 1, 5000);  // 5 second timeout
        if (ret <= 0)
          return false;  // timeout or error
        continue;        // retry write
      }
      return false;
    }
    data += written;
    len -= static_cast<size_t>(written);
  }
  return true;
}

/// Read a complete eibd message (length-prefixed) from the socket.
/// Uses an internal buffer to handle partial reads in non-blocking mode.
/// This is an iterative (non-recursive) implementation to avoid stack overflow
/// on busy KNX buses with many partial reads.
/// @param fd Socket file descriptor.
/// @param buffer Accumulated read buffer (consumed as messages are parsed).
///              Enforced maximum size of kMaxReadBufferSize to prevent memory leaks.
/// @return Complete message bytes (including 2-byte length header), or std::nullopt.
std::optional<std::vector<uint8_t>> read_message(int fd, std::vector<uint8_t>& buffer) {
  while (true) {
    // Try to extract a complete message from the buffer (pure function, no I/O)
    auto msg = try_extract_message(buffer);
    if (msg.has_value())
      return msg;

    // Need more data — read from socket
    uint8_t tmp[4096];
    ssize_t n = ::read(fd, tmp, sizeof(tmp));
    if (n > 0) {
      // Enforce maximum buffer size to prevent unbounded memory growth.
      // If the buffer would exceed the limit, discard the oldest data first.
      size_t new_size = buffer.size() + static_cast<size_t>(n);
      if (new_size > kMaxReadBufferSize) {
        size_t excess = new_size - kMaxReadBufferSize;
        // Discard from the front — this may lose a partial message,
        // but prevents OOM on a constrained system.
        if (excess >= buffer.size()) {
          buffer.clear();
        } else {
          buffer.erase(buffer.begin(), buffer.begin() + static_cast<ptrdiff_t>(excess));
        }
      }
      buffer.insert(buffer.end(), tmp, tmp + n);
      // Loop back to try parsing again with new data
      continue;
    }
    if (n == 0) {
      return std::nullopt;  // EOF — connection closed
    }
    // n < 0
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return std::nullopt;  // No data available in non-blocking mode
    }
    if (errno == EINTR) {
      continue;  // Retry on signal
    }
    return std::nullopt;  // Real error
  }
}

}  // namespace

bool KnxdClient::open_group_socket(bool write_only) {
  if (!is_connected())
    return false;

  impl_->write_only_ = write_only;

  uint8_t wo_byte = write_only ? 0xFF : 0x00;
  std::vector<uint8_t> data = {wo_byte};
  auto msg = build_eibd_message(EibMessageType::OPEN_GROUPCON, data);

  DebugLog::knxd_send("open_group_socket", "-",
                      write_only ? "write_only=true" : "write_only=false");

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
  if (!is_connected() || !impl_->group_socket_open) {
    // Attempt transparent reconnect once
    if (!reconnect())
      return false;
  }

  auto addr_str = KnxGroupAddress::from_eibaddr(group_addr).to_string();

  DebugLog::knxd_send("group_packet", addr_str, "apdu=" + hex_encode(apdu.data(), apdu.size()));

  std::vector<uint8_t> data;
  data.reserve(2 + apdu.size());
  data.push_back(static_cast<uint8_t>((group_addr >> 8) & 0xFF));
  data.push_back(static_cast<uint8_t>(group_addr & 0xFF));
  data.insert(data.end(), apdu.begin(), apdu.end());

  auto msg = build_eibd_message(EibMessageType::GROUP_PACKET, data);
  return write_all(impl_->fd, msg.data(), msg.size());
}

std::optional<std::vector<uint8_t>> KnxdClient::cache_read(uint16_t group_addr, bool nowait) {
  if (!is_connected()) {
    // Attempt transparent reconnect once
    if (!reconnect())
      return std::nullopt;
  }

  auto addr_str = KnxGroupAddress::from_eibaddr(group_addr).to_string();

  uint16_t msg_type = nowait ? EibMessageType::CACHE_READ_NOWAIT : EibMessageType::CACHE_READ;
  std::vector<uint8_t> data = {static_cast<uint8_t>((group_addr >> 8) & 0xFF),
                               static_cast<uint8_t>(group_addr & 0xFF)};

  DebugLog::knxd_send("cache_read", addr_str, nowait ? "nowait=true" : "nowait=false");

  auto msg = build_eibd_message(msg_type, data);
  if (!write_all(impl_->fd, msg.data(), msg.size()))
    return std::nullopt;

  // Read responses until we get a matching type.
  // Unsolicited APDU_PACKET telegrams may arrive between request and response;
  // we buffer them back so poll_group_telegram() can consume them later.
  std::vector<std::vector<uint8_t>> buffered_telegrams;

  while (true) {
    auto resp = read_message(impl_->fd, impl_->read_buffer_);
    if (!resp)
      return std::nullopt;  // EOF or no data

    uint16_t resp_type;
    std::vector<uint8_t> resp_data;
    if (!parse_eibd_message(*resp, resp_type, resp_data))
      continue;  // malformed message, try next

    if (resp_type == msg_type && resp_data.size() >= 6) {
      // This is our cache response.
      // Re-queue any buffered unsolicited telegrams back into read_buffer_
      // so they can be consumed by subsequent poll_group_telegram() calls.
      for (auto& tele : buffered_telegrams) {
        impl_->read_buffer_.insert(impl_->read_buffer_.begin(), tele.begin(), tele.end());
      }
      // Response format: src(2) + dst(2) + apdu_data...
      auto result_data = std::vector<uint8_t>(resp_data.begin() + 4, resp_data.end());
      DebugLog::knxd_recv("cache_read", addr_str,
                          hex_encode(result_data.data(), result_data.size()));
      return result_data;
    }

    if (resp_type == EibMessageType::APDU_PACKET) {
      // Unsolicited telegram — buffer it for later, keep reading
      buffered_telegrams.push_back(std::move(*resp));
      continue;
    }

    // Unknown message type — discard and keep reading
  }
}

int KnxdClient::get_fd() const {
  return impl_->fd;
}

void KnxdClient::set_nonblocking(bool enable) {
  if (impl_->fd < 0)
    return;
  int flags = ::fcntl(impl_->fd, F_GETFL, 0);
  if (flags < 0)
    return;
  if (enable) {
    ::fcntl(impl_->fd, F_SETFL, flags | O_NONBLOCK);
  } else {
    ::fcntl(impl_->fd, F_SETFL, flags & ~O_NONBLOCK);
  }
}

bool KnxdClient::poll_group_telegram(uint16_t& out_group_addr, std::vector<uint8_t>& out_apdu) {
  if (!is_connected()) {
    // Attempt transparent reconnect once
    if (!reconnect())
      return false;
  }

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

    DebugLog::knxd_recv("apdu_packet", KnxGroupAddress::from_eibaddr(out_group_addr).to_string(),
                        hex_encode(out_apdu.data(), out_apdu.size()));

    return true;
  }

  return false;
}

}  // namespace cvknxd
