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

#include "knxd_client.h"

namespace cvknxd {

/// Connect to knxd with exponential backoff retry.
/// Attempts to connect immediately, then retries on failure with increasing delays.
///
/// Standard retry (has_worked_before=false):
///   try -> wait 500ms -> try -> wait 1s -> try -> wait 2s -> try  (4 attempts total)
///
/// Extended retry (has_worked_before=true):
///   try -> wait 500ms -> try -> wait 1s -> try -> wait 2s -> try
///   -> wait 4s -> try -> wait 8s -> try  (6 attempts total)
///
/// @param knxd The knxd client interface to connect through.
/// @param socket_path Path to the knxd Unix socket.
/// @param has_worked_before If true, use extended retry delays (up to 8s).
/// @param custom_delays_ms Optional array of retry delays in milliseconds.
///        When nullptr (default), uses built-in defaults:
///        {500, 1000, 2000, 4000, 8000}.
///        Must have at least 5 elements when provided.
/// @return true if connection succeeded.
[[nodiscard]] bool connect_knxd_with_retry(KnxdClientInterface& knxd, const char* socket_path,
                                           bool has_worked_before,
                                           const int* custom_delays_ms = nullptr);

}  // namespace cvknxd
