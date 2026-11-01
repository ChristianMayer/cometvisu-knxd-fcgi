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
 * @file cache_reader.h
 * @brief The knxd -> SharedGroupCache pump (one dedicated process).
 *
 * The cache reader is the only process that reads group telegrams from knxd.
 * Its loop must be resilient: knxd can be absent at startup, restart at any
 * time ("Link down, terminating"), or disappear for hours.  The loop therefore
 * reconnects forever with an exponential backoff and never spins — a busy loop
 * would burn a full CPU core, which is unacceptable on an embedded system.
 *
 * Extracted from main.cpp so that the reconnect/backoff behaviour is unit
 * testable with MockKnxdClient.
 */

#ifndef COMETVISU_KNXD_FCGI_CACHE_READER_H_
#define COMETVISU_KNXD_FCGI_CACHE_READER_H_

#include <functional>
#include <string>

namespace cvknxd {

class KnxdClientInterface;
class SharedGroupCache;

/// Tunables for run_cache_reader().  Defaults are production values; tests
/// use very small values to keep the runtime short.
struct CacheReaderOptions {
  /// Path of the knxd Unix socket.
  std::string socket_path = "/run/knx";
  /// poll() slice while connected.  Also the maximum latency of the stop check.
  int wait_timeout_ms = 1000;
  /// First reconnect delay after a failure.
  int reconnect_backoff_ms = 500;
  /// Upper bound for the reconnect delay (backoff doubles up to this value).
  int max_reconnect_backoff_ms = 30000;
  /// Granularity of interruptible sleeps between reconnect attempts.
  int sleep_slice_ms = 100;
};

/// Pump group telegrams from knxd into the shared cache until should_stop()
/// returns true.
///
/// Guarantees:
///   - never busy-loops, even when knxd is missing or the connection dies;
///   - retries the connection forever with exponential backoff;
///   - resumes pushing telegrams as soon as knxd answers again.
///
/// @param knxd Client to use (must not be connected yet; the loop owns it).
/// @param cache Shared cache that receives the telegrams.
/// @param options Timing and socket-path configuration.
/// @param should_stop Cooperative stop predicate, evaluated frequently.
void run_cache_reader(KnxdClientInterface& knxd, SharedGroupCache& cache,
                      const CacheReaderOptions& options, const std::function<bool()>& should_stop);

}  // namespace cvknxd

#endif  // COMETVISU_KNXD_FCGI_CACHE_READER_H_
