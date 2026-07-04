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

#include "read_handler.h"

#include <poll.h>

#include <cerrno>
#include <chrono>
#include <set>

#include "knxd/knxd_client.h"
#include "knxd/knxd_protocol.h"
#include "state/session_store.h"
#include "util/hex.h"
#include "util/json_builder.h"
#include "util/query_string.h"

namespace cvknxd {

ReadHandler::ReadHandler(KnxdClientInterface& knxd, SessionStore& sessions,
                         int longpoll_timeout_sec)
    : knxd_(knxd), sessions_(sessions), longpoll_timeout_sec_(longpoll_timeout_sec) {}

std::string ReadHandler::generate_index() {
  return std::to_string(index_counter_++);
}

std::optional<int> ReadHandler::parse_timeout(std::string_view t_str) {
  if (t_str.empty())
    return std::nullopt;
  try {
    std::string s{t_str};
    size_t pos = 0;
    int val = std::stoi(s, &pos);
    if (pos != s.size())
      return std::nullopt;  // trailing garbage
    return val;
  } catch (const std::invalid_argument&) {
    return std::nullopt;
  } catch (const std::out_of_range&) {
    return std::nullopt;
  }
}

ReadResult ReadHandler::handle(std::string_view query_string) {
  QueryString params{query_string};
  ReadResult result;

  // ---- Get addresses first (before session check, so 400 takes priority) ----
  auto addresses = params.get_all("a");
  if (addresses.empty()) {
    result.http_status = 400;
    result.body = "{}";
    return result;
  }

  // ---- Session validation ----
  if (auto s_opt = params.get("s")) {
    if (!sessions_.is_valid(*s_opt)) {
      result.http_status = 401;
      result.body = "{}";
      return result;
    }
  }

  // ---- Parse timeout ----
  std::optional<int> timeout;
  if (auto t_opt = params.get("t")) {
    timeout = parse_timeout(*t_opt);
    if (!timeout.has_value()) {
      result.http_status = 400;
      result.body = "{}";
      return result;
    }
  }

  // ---- Collect EIB addresses ----
  std::set<uint16_t> eib_addrs;
  for (auto addr_str : addresses) {
    auto parsed = KnxAddress::from_cometvisu(addr_str);
    if (parsed) {
      eib_addrs.insert(parsed->group.to_eibaddr());
    }
  }

  if (eib_addrs.empty()) {
    result.http_status = 404;
    result.body = "{}";
    return result;
  }

  // ---- Long-poll mode: no timeout parameter ----
  if (!timeout.has_value()) {
    // ---- Efficient poll-based wait on knxd socket ----
    int knxd_fd = knxd_.get_fd();
    if (knxd_fd < 0) {
      // No real fd (e.g., mock): fall back to quick poll
      uint16_t recv_addr;
      std::vector<uint8_t> apdu_data;
      if (knxd_.poll_group_telegram(recv_addr, apdu_data)) {
        if (eib_addrs.find(recv_addr) != eib_addrs.end()) {
          JsonBuilder resp;
          resp.start_object();
          resp.add_key("d");
          resp.start_object();
          auto cv_addr = KnxAddress{"KNX", KnxGroupAddress::from_eibaddr(recv_addr)};
          resp.add_string(cv_addr.to_cometvisu(), hex_encode(apdu_data.data(), apdu_data.size()));
          resp.end_object();
          resp.add_string("i", generate_index());
          resp.end_object();
          result.body = resp.take();
          return result;
        }
      }
      // Timeout with no data
      JsonBuilder resp;
      resp.start_object();
      resp.add_key("d");
      resp.start_object();
      resp.end_object();
      resp.add_string("i", generate_index());
      resp.end_object();
      result.body = resp.take();
      return result;
    }

    // Use poll() on the knxd socket — efficient, no CPU burning
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(longpoll_timeout_sec_);

    while (true) {
      auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                           deadline - std::chrono::steady_clock::now())
                           .count();
      if (remaining <= 0)
        break;  // timeout

      struct pollfd pfd;
      pfd.fd = knxd_fd;
      pfd.events = POLLIN;
      pfd.revents = 0;

      int poll_ret = ::poll(&pfd, 1, static_cast<int>(remaining));
      if (poll_ret < 0) {
        if (errno == EINTR)
          continue;
        break;  // error
      }
      if (poll_ret == 0)
        break;  // timeout

      if (pfd.revents & POLLIN) {
        uint16_t recv_addr;
        std::vector<uint8_t> apdu_data;

        while (knxd_.poll_group_telegram(recv_addr, apdu_data)) {
          // cache_.update() and long-poll notify are handled by the
          // telegram callback invoked inside poll_group_telegram()

          if (eib_addrs.find(recv_addr) != eib_addrs.end()) {
            JsonBuilder resp;
            resp.start_object();
            resp.add_key("d");
            resp.start_object();
            auto cv_addr = KnxAddress{"KNX", KnxGroupAddress::from_eibaddr(recv_addr)};
            resp.add_string(cv_addr.to_cometvisu(), hex_encode(apdu_data.data(), apdu_data.size()));
            resp.end_object();
            resp.add_string("i", generate_index());
            resp.end_object();
            result.body = resp.take();
            return result;
          }
        }
      }

      // Handle disconnect/error — avoid tight spin loop
      if (pfd.revents & (POLLHUP | POLLERR)) {
        break;
      }
    }

    // Long-poll timeout: return empty
    JsonBuilder resp;
    resp.start_object();
    resp.add_key("d");
    resp.start_object();
    resp.end_object();
    resp.add_string("i", generate_index());
    resp.end_object();
    result.body = resp.take();
    return result;
  }

  // ---- Non-long-poll: timeout provided — query knxd's built-in cache ----
  int t_val = *timeout;
  JsonBuilder json;
  json.start_object();
  json.add_key("d");
  json.start_object();

  bool any_found = false;

  for (auto addr : eib_addrs) {
    std::optional<std::vector<uint8_t>> data;

    if (t_val > 0 || t_val == 0) {
      // Use knxd's built-in cache (delegates to knxd's group cache)
      data = knxd_.cache_read(addr, (t_val == 0) ? false : true);
    } else {
      // t_val < 0: cache only, non-blocking
      data = knxd_.cache_read(addr, true);
    }

    if (data) {
      auto cv_addr = KnxAddress{"KNX", KnxGroupAddress::from_eibaddr(addr)};
      json.add_string(cv_addr.to_cometvisu(), hex_encode(data->data(), data->size()));
      any_found = true;
    }
  }

  json.end_object();
  json.add_string("i", generate_index());
  json.end_object();

  if (!any_found) {
    result.http_status = 404;
  }

  result.body = json.take();
  return result;
}

}  // namespace cvknxd
