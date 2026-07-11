# AGENTS.md — Instructions for LLM Coding Agents

This file provides guidelines for AI coding agents working on the
`cometvisu-knxd-fcgi` project.

## Development Methodology

**Test-Driven Development (TDD) is mandatory.** The cycle is:

1. **Write a failing test** — define the expected behavior.
2. **Write the minimum code** to make the test pass.
3. **Refactor** — clean up while keeping tests green.
4. **Repeat.**

Never write implementation code before the corresponding test exists.

## Build & Test Commands

```bash
# Configure (once)
cmake -B build -DCMAKE_BUILD_TYPE=Debug

# Build
cmake --build build -j$(nproc)

# Run all tests
ctest --test-dir build --output-on-failure

# Run a specific test
ctest --test-dir build -R test_query_string --output-on-failure

# Run with verbose output
ctest --test-dir build -V
```

## Coding Style

- **C++20** standard.
- Clang-format: Google style, 100 character line limit.
- **Indentation: 2 spaces. Tab width: 8 spaces.** Never use tabs.
- `.clang-format` and `.clang-tidy` are in the repository root.
- Run `clang-format -i <file>` before committing.
- Header guards: `#pragma once` (modern and supported everywhere).
- Namespace: `cvknxd` (CometVisu KNX Daemon).

## Copyright Header

**Every source file (.cpp, .h, CMakeLists.txt) MUST start with the GPLv3
copyright header.** This is mandatory for all files, now and in the future.

For C++ files (`.cpp`, `.h`):
```cpp
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
```

For CMake files (`CMakeLists.txt`), use `#` instead of `//`:
```cmake
# Copyright (C) 2026 Christian Mayer and the CometVisu contributors
#
# This program is free software: ...
# ...
```

The copyright header must be followed by exactly one blank line before the
actual file content (e.g., `#pragma once` or `#include`).

## File Organization

| Directory        | Purpose                                      |
|------------------|----------------------------------------------|
| `src/`           | Production source code                       |
| `src/fcgi/`      | FastCGI protocol implementation              |
| `src/router/`    | URL routing and handler dispatch             |
| `src/handlers/`  | `/l`, `/r`, `/w` endpoint handlers           |
| `src/knxd/`      | knxd Unix socket client and eibd protocol    |
| `src/state/`     | Session store, address cache                  |
| `src/util/`      | Query string parser, JSON builder, hex utils |
| `tests/unit/`    | Unit tests (mocked dependencies)             |
| `tests/integration/` | Integration tests (real or fake socket)  |
| `tests/e2e/`     | End-to-end tests (full FCGI cycle)           |
| `mocks/`         | Mock/fake implementations for testing        |

## Key Design Decisions

1. **Multi-threaded FCGI server**: The FCGI accept loop uses multiple worker
   threads, each running its own `FCGX_Accept_r()` on the shared listen socket.
   The OS serializes accept calls across threads. This is critical because
   long-poll `/r` requests block for up to 300 seconds — without threading,
   one long-poll would block all other clients. Thread count is configurable
   (default: 4). All shared state (KnxdClient, SessionStore) is protected by
   `std::mutex`.

2. **Knxd connection is persistent**: A single Unix socket connection to knxd
   is opened at startup and reused for all requests. The client opens a "Group
   Socket" (not a T_Group tunnel per address) for listening to group telegrams.
   Access to the knxd socket is serialized via mutex since multiple worker
   threads share the same connection.

3. **Address cache**: Maintains a `std::unordered_map<group_addr, CacheEntry>`
   where `CacheEntry` has `value`, `last_updated` timestamp.

4. **Long-poll**: For `/r` requests without timeout, the handler enters a
   `poll()`-based wait loop on the knxd socket file descriptor. The kernel puts
   the process to sleep until data arrives or the configurable timeout expires,
   burning zero CPU. Incoming telegrams update the cache via the telegram
   callback and matching requests wake immediately.

5. **No external JSON library**: The JSON responses are simple enough to build
   with a minimal, purpose-built `JsonBuilder`.

6. **No external HTTP parser**: FastCGI provides parsed `QUERY_STRING` via
   `FCGI_PARAMS`. We parse the query string ourselves with `QueryString`.

## Knxd Protocol Details

The eibd client protocol over Unix socket:

### Wire Format
```
[2 bytes: payload length (big-endian)] [payload]
```

### Payload Structure
```
[2 bytes: message type] [message-specific data]
```

### Key Message Types (from `eibtypes.h`)

| Constant                | Value    | Purpose                        |
|-------------------------|----------|--------------------------------|
| `EIB_OPEN_GROUPCON`     | `0x0026` | Open group socket connection   |
| `EIB_GROUP_PACKET`      | `0x0027` | Send group telegram            |
| `EIB_APDU_PACKET`       | `0x0025` | Received group telegram        |
| `EIB_OPEN_T_GROUP`      | `0x0022` | Open T_Group (for read)        |
| `EIB_CACHE_READ`        | `0x0074` | Read from group cache          |
| `EIB_CACHE_READ_NOWAIT` | `0x0075` | Read from group cache (no wait)|

### Group Address Format
KNX three-level address `X/Y/Z` → 16-bit:
```
uint16_t addr = (X << 11) | (Y << 8) | Z;
```

### APDU for Group Value Write
```
byte 0: 0x00
byte 1: 0x80 | (first_data_byte & 0x3F)   // 0x80 = write, 0x40 = response, 0x00 = read
bytes 2..N: remaining data bytes
```

For multi-byte values (e.g., DPT 9.001 temperature: 2 bytes):
```
0x00 0x80 0x0c 0x6f   → A_GroupValue_Write, data=0c6f
```

### Group Socket Open (EIB_OPEN_GROUPCON)
```
[0x00 0x26] [write_only_byte]
```
Response: empty success or error message.

### Sending a Group Packet (EIB_GROUP_PACKET)
```
[0x00 0x27] [dest_addr_hi] [dest_addr_lo] [APDU bytes...]
```

### Receiving a Group Packet (EIB_APDU_PACKET)
```
[0x00 0x25] [src_pa_hi] [src_pa_lo] [dst_ga_hi] [dst_ga_lo] [APDU bytes...]
```
Note: The destination group address (dst_ga) is at offset 2-3 of the payload
(after the 2-byte type), NOT at offset 0-1 (which is the source physical address).

### Cache Read Response (EIB_CACHE_READ / EIB_CACHE_READ_NOWAIT)
```
[type:2] [src:2] [dst:2] [apdu_data...]
```
- Cache hit: payload size >= 6 (src + dst + at least 2 APDU bytes)
- Cache miss: payload size == 4 (src + dst only, no APDU data)

## APDU Decoding

Given APDU bytes `[b0, b1, b2, ...]`:
- `b0` = always 0x00 for group value
- `b1 & 0xC0` = APCI (Application Layer Protocol Control Information):
  - `0x00` = A_GroupValue_Read
  - `0x40` = A_GroupValue_Response
  - `0x80` = A_GroupValue_Write
- For 1-byte values: `b1 & 0x3F` is the value
- For multi-byte values: `b2, b3, ...` are the remaining data bytes

## Hex Encoding for CometVisu

KNX data values are transmitted as hex strings in CometVisu JSON:
- Single byte `0x42` → `"42"`
- Two bytes `0x0c 0x6f` → `"0c6f"`
- Always lowercase, no spaces, no `0x` prefix.

## Test Mocking Strategy

For unit tests of knxd-dependent components, use dependency injection:
- `KnxdClient` has a virtual `send()` / `recv()` that can be mocked.
- Or use a real Unix socket pair in tests with a mock server responding.

For integration tests, use:
- A real knxd instance if available.
- A fake socket server (`mocks/mock_knxd_socket.h`) that speaks enough of the
  eibd protocol to satisfy test scenarios.

## Reference: Existing CometVisu Backend Implementations

- PHP: https://github.com/CometVisu/cometvisu-bsp/blob/master/src/php/helper/KnxHelper.php
- Perl (linkback): part of the CometVisu distribution

These handle address parsing, hex conversion, and the full protocol flow.

## Logging

Use simple stderr-based logging with levels:
- `ERROR`: Fatal problems
- `WARN`: Recoverable issues
- `INFO`: Normal operational messages
- `DEBUG`: Detailed diagnostics

Format: `[LEVEL] [timestamp] message`
