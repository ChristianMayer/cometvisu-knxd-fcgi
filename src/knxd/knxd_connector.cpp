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

#include "knxd_connector.h"

#include <unistd.h>

#include <iostream>

namespace cvknxd {

bool connect_knxd_with_retry(KnxdClientInterface& knxd, const char* socket_path,
                             bool has_worked_before, const int* custom_delays_ms) {
  // Retry delays in milliseconds applied BETWEEN attempts.
  // Standard (has_worked_before=false): 500ms, 1s, 2s  → 4 attempts total
  // Extended (has_worked_before=true):  500ms, 1s, 2s, 4s, 8s → 6 attempts total
  const int default_delays_ms[] = {500, 1000, 2000, 4000, 8000};
  const int* delays_ms = custom_delays_ms != nullptr ? custom_delays_ms : default_delays_ms;
  int num_retries = has_worked_before ? 5 : 3;
  int total_attempts = num_retries + 1;

  for (int attempt = 1; attempt <= total_attempts; ++attempt) {
    if (knxd.connect(socket_path)) {
      if (attempt > 1) {
        std::cout << "[INFO] knxd connection succeeded after " << attempt << " attempts\n";
      }
      return true;
    }
    if (attempt < total_attempts) {
      int delay = delays_ms[attempt - 1];
      std::cerr << "[WARN] knxd connection attempt " << attempt << "/" << total_attempts
                << " failed, retrying in " << delay << "ms...\n";
      ::usleep(static_cast<useconds_t>(delay) * 1000);
    }
  }

  std::cerr << "[ERROR] knxd connection failed after " << total_attempts
            << " attempts, giving up\n";
  return false;
}

}  // namespace cvknxd
