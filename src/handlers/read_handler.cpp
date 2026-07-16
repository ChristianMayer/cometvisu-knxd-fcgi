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

#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

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

std::optional<int> ReadHandler::parse_timeout(std::string_view t_str) {
  if (t_str.empty()) {
    return std::nullopt;
  }
  int val = 0;
  const auto [ptr, ec] = std::from_chars(t_str.data(), t_str.data() + t_str.size(), val);
  if (ec != std::errc{}) {
    return std::nullopt;
  }
  // Check for trailing garbage
  if (ptr != t_str.data() + t_str.size()) {
    return std::nullopt;
  }
  return val;
}

ReadResult ReadHandler::handle(std::string_view query_string) {
  const QueryString params{query_string};
  ReadResult result;

  // ---- Get addresses first (before anything else, so 400 takes priority) ----
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

  // ---- Parse timeout (t parameter) ----
  // Original semantics: t is a simple timeout in seconds for the poll loop.
  // If t is not specified, use the default longpoll timeout.
  // If t == 0: force initial read (lastpos=0) and set timeout to 1 second.
  int timeout_sec = longpoll_timeout_sec_;
  if (auto t_opt = params.get("t")) {
    auto parsed = parse_timeout(*t_opt);
    if (!parsed.has_value()) {
      result.http_status = 400;
      result.body = "{}";
      return result;
    }
    timeout_sec = *parsed;
  }

  // ---- Parse position (i parameter) ----
  // i is the last known position (like "lastpos" in the original).
  // 0 means the client has no prior state.
  uint32_t lastpos = 0;
  if (auto i_opt = params.get("i")) {
    auto parsed = parse_timeout(*i_opt);  // reuse int parser
    if (parsed.has_value() && *parsed >= 0) {
      lastpos = static_cast<uint32_t>(*parsed);
    }
    // Invalid i is ignored (not an error)
  }

  // ---- t=0 special handling: force initial read ----
  if (timeout_sec == 0) {
    lastpos = 0;
    timeout_sec = 1;
  }

  // ---- Collect EIB addresses and build lookup set ----
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

  // Helper: build the key for an address in the JSON response.
  auto addr_key = [](uint16_t eib_addr) -> std::string {
    return KnxAddress{std::string{KnxAddress::get_default_namespace()},
                      KnxGroupAddress::from_eibaddr(eib_addr)}
        .to_cometvisu();
  };

  JsonBuilder json;
  json.start_object();
  json.add_key("d");
  json.start_object();
  bool written = false;

  // Track which addresses we've already included (deduplication).
  std::set<uint16_t> already_written;

  // ---- Initial read (if lastpos == 0) ----
  // Reads ALL requested addresses from the knxd cache synchronously.
  // This matches the original: for (i = 0; i < UINT16; i++) { if (subscribed) { ... } }
  // For addresses NOT found in cache, a GroupValueRead telegram is sent to
  // query the device's current value. The poll loop below will catch the
  // responses.
  if (lastpos == 0) {
    for (auto addr : eib_addrs) {
      auto data = knxd_.cache_read(addr, true);  // nowait (cache_read filters out Read APDUs)
      if (data) {
        json.add_string(addr_key(addr), hex_encode(data->data(), data->size()));
        already_written.insert(addr);
        written = true;
      } else {
        // Cache miss: send GroupValueRead to query the device's current value.
        knxd_.send_group_packet(addr, build_apdu(ApduType::Read, {}));
      }
    }
  }

  // ---- Poll loop (COMET/long-poll) ----
  // Original: while ((!written || lastpos < 1) && difftime(time(NULL), tstart) < timeout)
  // - Continue while nothing written OR this was an initial request
  // - Stop when timeout elapses
  auto tstart = std::chrono::steady_clock::now();

  // Retry counter for transient cache_last_updates_2 failures.
  // When cache_last_updates_2 returns nullopt but the main connection is
  // alive, it may be a transient cache connection failure (e.g., knxd
  // temporarily not accepting new connections). Retry a few times with
  // delays before giving up. This counter is scoped to the outer while
  // loop — each successful call resets our confidence.
  int nullopt_retries = 0;
  static constexpr int kMaxNulloptRetries = 5;

  while ((!written || lastpos < 1) && timeout_sec > 0) {
    // Calculate remaining time
    auto elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - tstart)
            .count();
    int remaining = timeout_sec - static_cast<int>(elapsed);
    if (remaining <= 0)
      break;

    // Use cache_last_updates_2 for position-based polling.
    // This is the equivalent of the original EIB_Cache_LastUpdates().
    // The original blocks for the full timeout if no updates are available.
    // The KnxdClient implementation handles internal reconnection transparently,
    // but if knxd is still down after the internal retry, we attempt a full
    // reconnect here and continue the loop with the remaining time budget.
    auto call_start = std::chrono::steady_clock::now();
    auto updates = knxd_.cache_last_updates_2(lastpos, remaining);
    if (!updates.has_value()) {
      // cache_last_updates_2 can return nullopt for three reasons:
      // 1. Transient cache connection failure — retry with delay.
      // 2. Connection error — reconnect and retry if time remains.
      // 3. Timeout (blocks for remaining time) — break normally.
      //
      // Distinguish by checking connection health and call duration.
      auto call_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - call_start)
                                 .count();

      if (!knxd_.is_connected()) {
        // Connection is dead — reconnect and retry if time remains
        auto now = std::chrono::steady_clock::now();
        auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(now - tstart).count();
        if (elapsed_sec < timeout_sec && (timeout_sec - elapsed_sec) > 1) {
          knxd_.reconnect();
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
          continue;
        }
      }

      // Connection is alive but cache_last_updates_2 returned nullopt.
      // This can happen due to transient cache connection failures
      // (e.g., knxd temporarily not accepting new connections) or
      // when the mock has no queued results. Retry a limited number
      // of times before giving up to avoid an infinite loop.
      if (remaining > 0 && nullopt_retries < kMaxNulloptRetries) {
        nullopt_retries++;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }
      break;
    }

    // Successful call — reset retry counter
    nullopt_retries = 0;

    uint32_t prev_lastpos = lastpos;
    lastpos = updates->new_position;

    // Process all changed addresses
    for (auto changed_addr : updates->changed_addresses) {
      // Only include subscribed addresses, and deduplicate
      if (eib_addrs.find(changed_addr) == eib_addrs.end())
        continue;
      if (already_written.find(changed_addr) != already_written.end())
        continue;

      // Read the current value from cache (cache_read filters out Read APDUs)
      auto data = knxd_.cache_read(changed_addr, true);  // nowait
      if (data) {
        json.add_string(addr_key(changed_addr), hex_encode(data->data(), data->size()));
        already_written.insert(changed_addr);
        written = true;
      }
    }

    if (written)
      break;

    // Guard against busy-loop: if no progress was made (position didn't
    // advance and no changes found), knxd's internal timeout (~1s) expired
    // with no updates. Sleep briefly to avoid tight CPU spinning when knxd
    // returns empty results immediately (e.g., on a fresh bus with no
    // traffic). The outer while loop handles the overall timeout via
    // elapsed/remaining calculation.
    if (lastpos == prev_lastpos && updates->changed_addresses.empty()) {
      // knxd returned "no updates" — pause briefly then continue polling.
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }

    // Guard against busy-loop on a busy bus: if the position advanced due
    // to telegrams for non-subscribed addresses, cache_last_updates_2
    // returns immediately on every call, causing a tight CPU-spinning loop.
    // A brief pause lets the bus settle and keeps us responsive.
    if (lastpos != prev_lastpos && !updates->changed_addresses.empty()) {
      // Position advanced but our addresses didn't change.
      // Sleep briefly to avoid CPU spinning, then continue polling
      // with the updated position.
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  json.end_object();  // d
  json.add_number("i", lastpos);
  json.end_object();  // root

  result.body = json.take();
  return result;
}

}  // namespace cvknxd
