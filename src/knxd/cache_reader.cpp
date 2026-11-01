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
 * @file cache_reader.cpp
 * @brief Implementation of the resilient knxd -> SharedGroupCache pump.
 */

#include "knxd/cache_reader.h"

#include <time.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>

#include "knxd/knxd_client.h"
#include "knxd/knxd_protocol.h"
#include "state/shared_group_cache.h"
#include "util/debug_log.h"
#include "util/hex.h"

namespace cvknxd {
namespace {

/// Minimum interval between two "knxd unreachable" log lines.  knxd may be
/// gone for days; logging every attempt would flood the container log.
inline constexpr int kRetryLogIntervalMs = 30000;

/// Sleep for @p duration_ms, but wake up early when should_stop() becomes true.
/// Sleeps in slices so a shutdown never waits for a full backoff period.
void sleep_interruptible(int duration_ms, int slice_ms, const std::function<bool()>& should_stop) {
  const int slice = std::max(1, slice_ms);
  int remaining = duration_ms;
  while (remaining > 0) {
    if (should_stop && should_stop()) {
      return;
    }
    const int chunk = std::min(remaining, slice);
    struct timespec request{};
    request.tv_sec = chunk / 1000;
    request.tv_nsec = static_cast<long>(chunk % 1000) * 1000000L;
    // EINTR (signal) is expected — the loop re-checks the stop flag.
    (void)::nanosleep(&request, nullptr);
    remaining -= chunk;
  }
}

/// Push every pending group telegram into the shared cache.
/// @return number of telegrams that were pushed.
int drain_telegrams(KnxdClientInterface& knxd, SharedGroupCache& cache) {
  int pushed = 0;
  uint16_t addr = 0;
  std::vector<uint8_t> apdu;
  while (knxd.poll_group_telegram(addr, apdu)) {
    ApduType apdu_type{};
    std::vector<uint8_t> value_data;
    if (!parse_apdu(apdu, apdu_type, value_data) || apdu_type == ApduType::Read) {
      continue;
    }
    cache.push(addr, value_data);
    DebugLog::knxd_recv("cache_push", KnxGroupAddress::from_eibaddr(addr).to_string(),
                        hex_encode(value_data.data(), value_data.size()));
    ++pushed;
  }
  return pushed;
}

}  // namespace

void run_cache_reader(KnxdClientInterface& knxd, SharedGroupCache& cache,
                      const CacheReaderOptions& options, const std::function<bool()>& should_stop) {
  const int initial_backoff_ms = std::max(1, options.reconnect_backoff_ms);
  const int max_backoff_ms = std::max(initial_backoff_ms, options.max_reconnect_backoff_ms);
  const int wait_timeout_ms = std::max(1, options.wait_timeout_ms);

  int backoff_ms = initial_backoff_ms;
  bool connected = false;

  bool logged_failure = false;
  std::chrono::steady_clock::time_point last_log{};

  // Passing an empty std::function is allowed and means "never stop".
  const auto stop_requested = [&should_stop]() -> bool {
    return static_cast<bool>(should_stop) && should_stop();
  };

  // Rate-limited warning while knxd is unreachable.
  const auto log_retry = [&logged_failure, &last_log](const std::string& reason) {
    const auto now = std::chrono::steady_clock::now();
    const bool due =
        !logged_failure ||
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log).count() >=
            kRetryLogIntervalMs;
    if (due) {
      std::cerr << "[WARN] " << reason << " — retrying in the background\n";
      last_log = now;
      logged_failure = true;
    }
  };

  while (!stop_requested()) {
    // ---------------- (Re)connect ----------------
    if (!connected) {
      if (knxd.connect(options.socket_path) && knxd.open_group_socket(false)) {
        knxd.set_nonblocking(true);
        connected = true;
        backoff_ms = initial_backoff_ms;

        if (logged_failure) {
          std::cout << "[INFO] knxd connection restored at " << options.socket_path << "\n";
          std::cout << std::flush;  // stdout is block-buffered when redirected
          logged_failure = false;
        }
        continue;  // immediately look for pending telegrams
      }

      // Make sure a half-open connection does not survive the failed attempt.
      knxd.disconnect();

      log_retry("Cannot connect to knxd at " + options.socket_path);
      sleep_interruptible(backoff_ms, options.sleep_slice_ms, should_stop);
      backoff_ms = std::min(backoff_ms * 2, max_backoff_ms);
      continue;
    }

    // ---------------- Wait for group telegrams ----------------
    switch (knxd.wait_for_activity(wait_timeout_ms)) {
      case KnxdClientInterface::WaitResult::GroupData:
        if (drain_telegrams(knxd, cache) > 0) {
          backoff_ms = initial_backoff_ms;
        }
        break;

      case KnxdClientInterface::WaitResult::Timeout:
        // Nothing to do — just check the stop flag again.
        break;

      case KnxdClientInterface::WaitResult::Error:
        // Connection lost (knxd restarted / "Link down, terminating").
        // Never fall through to a busy loop: back off, then reconnect.
        std::cerr << "[WARN] Lost connection to knxd at " << options.socket_path
                  << " — reconnecting\n";
        logged_failure = true;
        last_log = std::chrono::steady_clock::now();

        knxd.disconnect();
        connected = false;
        sleep_interruptible(backoff_ms, options.sleep_slice_ms, should_stop);
        backoff_ms = std::min(backoff_ms * 2, max_backoff_ms);
        break;
    }
  }
}

}  // namespace cvknxd
