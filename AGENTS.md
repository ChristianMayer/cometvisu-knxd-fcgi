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
| `src/state/`     | Session store, shared & local group caches    |
| `src/util/`      | Query string parser, JSON builder, hex utils |
| `tests/unit/`    | Unit tests (mocked dependencies)             |
| `tests/integration/` | Integration tests (real or fake socket)  |
| `tests/e2e/`     | End-to-end tests (full FCGI cycle)           |
| `mocks/`         | Mock/fake implementations for testing        |

## Key Design Decisions

1. **Fork-based worker pool**: The FCGI accept loop uses multiple forked
   worker processes, each running its own `FCGX_Accept_r()` loop on the
   shared listen socket. The OS serializes accept calls across processes.
   This is critical because long-poll `/r` requests block for up to 300
   seconds — without multiple workers, one long-poll would block all other
   clients. Worker count is configurable via `FCGI_WORKERS` (default: 20).
   Each worker opens its own knxd connection.
   All shared state (KnxdClient, SessionStore) is protected by
   `std::mutex`/`std::recursive_mutex` for thread-safe access within each
   process.
   **The parent supervises the pool and is the only process that reaps
   children** (no SIGCHLD handler that calls `waitpid()` — it would steal the
   exit statuses).  Dead workers and a dead cache reader are restarted with a
   crash-loop backoff (100 ms … 30 s).  The parent therefore keeps the listen
   socket open so replacements inherit it; closing it made every respawned
   worker fall back to spawn-fcgi mode and exit immediately.
   Fork-based workers avoid Docker < 20.10.10 seccomp incompatibility where
   `clone3`/`clone` with process-sharing flags (CLONE_VM, CLONE_THREAD) is
   blocked, preventing `std::thread` creation.

2. **Knxd connections**:
   - **Cache reader process**: A single dedicated child process opens a
     read-write group socket to knxd.  It continuously drains group telegrams
     (APDU_PACKET and GROUP_PACKET) and pushes them into the shared cache.
   - **Worker processes**: Each worker opens a **write-only** group socket
     (`open_group_socket(true)`) to knxd.  Workers only send group telegrams
     (via `/w` handler) — all group telegram reception goes through the cache
     reader.  This ensures only one knxd connection reads from the bus,
     eliminating duplicate processing and per-worker position divergence.
   - **Handshake deadline**: every `EIB_OPEN_GROUPCON` reply is read with a
     deadline (`KnxdClient::set_handshake_timeout_ms()`, default 5 s).  A knxd
     that accepts the connection but never answers must not block the caller —
     that used to hang `/w` requests forever.

3. **Shared-memory group cache (`SharedGroupCache`)**: A fixed-size
   open-addressing hash table (2048 entries) in `mmap(MAP_SHARED|MAP_ANONYMOUS)`
   memory, accessible by all worker processes.  Each entry stores the KNX group
   address, APDU value data (up to 14 bytes), a Unix timestamp, and the
   `pushed_at` position.  Synchronization:
   - `pthread_mutex_t` with `PTHREAD_PROCESS_SHARED` and `PTHREAD_MUTEX_ROBUST`
     protects all read/write access to cache entries.
   - `pthread_cond_t` with `PTHREAD_PROCESS_SHARED` and `CLOCK_MONOTONIC`
     enables zero-CPU blocking for long-poll readers.
   - `std::atomic<uint32_t> position` — lock-free reads, monotonically
     increasing across all workers.  The single source of truth for the `i`
     field in `/r` responses.
   - `std::atomic<uint32_t> generation` — incremented on each push, used by
     `wait_for_new_data()` for efficient change detection.

4. **Long-poll**: For `/r` requests, the ReadHandler blocks on the shared
   condition variable via `pthread_cond_timedwait()` instead of polling a knxd
   socket fd.  When the cache reader pushes new data, it broadcasts the
   condition variable — all blocked workers wake immediately (microsecond
   latency) and call `get_delta()` to find matching entries.  Zero CPU while
   waiting.  The `wait_for_new_data()` method handles spurious wakeups and
   robust-mutex recovery (EOWNERDEAD).

   Only `/r` takes a load-shedding/concurrency token; `/w` and `/l` never do,
   because a spurious 503 on a write surfaces in CometVisu as a failed switch
   operation.  Tokens are released via RAII (a throwing handler yields HTTP 500
   instead of terminating the worker) and the parent restores the token of a
   worker that died, so the pool cannot shrink permanently.

5. **KNX semantic correctness rules** — these are non-negotiable constraints
   that every handler implementation MUST satisfy:

   **Rule 1 — No duplicate delivery**: Every KNX telegram must be delivered
   to the client exactly once.  Duplicate delivery is a correctness bug
   because KNX telegrams carry semantic meaning beyond their data value:
   a "toggle" command toggles twice if delivered twice, a "scene trigger"
   triggers the scene twice, a "step" dimming command steps twice.  The
   handler must use `already_written` deduplication within a single response
   AND must never return data with a stale position (`i`) that would cause
   re-reporting of the same telegram on the next poll.

   **Rule 2 — Immediate delivery**: Group telegrams arriving on the knxd
   group socket must be processed without delay.  The cache reader process
   pushes them to the shared cache and broadcasts the condition variable
   immediately.  Blocked long-poll readers wake and query `get_delta()`
   with zero additional latency.

   **Rule 3 — Authoritative index**: The `i` value in every response must
   come from the shared cache's `position` counter.  This counter is
   `std::atomic<uint32_t>`, incremented atomically on each push, and shared
   across all worker processes via `mmap`.  The handler must never fabricate,
   locally infer, or manipulate the position.  The shared counter guarantees
   monotonicity — `i` never goes backward, preventing duplicate delivery
   across worker boundaries.

6. **No external JSON library**: The JSON responses are simple enough to build
   with a minimal, purpose-built `JsonBuilder`.

7. **No external HTTP parser**: FastCGI provides parsed `QUERY_STRING` via
   `FCGI_PARAMS`. We parse the query string ourselves with `QueryString`.

8. **Cache reader process**: A dedicated child process (forked before the
   worker pool) that owns the single knxd group socket connection.  It runs
   a `poll()`-based loop: wait for activity on the knxd fd, drain all pending
   group telegrams, parse APDU data, and push value data to the shared cache.
   Each push increments the shared position counter and broadcasts the
   condition variable to wake blocked long-poll readers.
   The loop lives in `run_cache_reader()` (`src/knxd/cache_reader.{h,cpp}`, unit
   tested via `MockKnxdClient`) and is resilient by design: knxd may be absent at
   startup or restart at any time (`Link down, terminating`), so every failure
   path reconnects forever with an exponential backoff (500 ms … 30 s) and an
   interruptible sleep.  It must never busy-loop — a spin here burns a full CPU
   core on an embedded system, which is exactly the bug this design prevents.
   `wait_for_activity()` therefore distinguishes `Timeout` (no data, connection
   fine) from `Error` (POLLHUP/POLLERR/POLLNVAL or not connected).

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

knxd also echoes injected telegrams back as GROUP_PACKET messages:
```
[0x00 0x27] [src_pa_hi] [src_pa_lo] [dst_ga_hi] [dst_ga_lo] [APDU bytes...]
```
Both message types are handled identically by the cache reader — the
payload structure is the same (src_pa + dst_ga + apdu).

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

For unit tests of the shared cache, use `SharedGroupCache::create()` which
allocates a real mmap'd region — this works fine in single-process tests.
For multi-instance tests (simulating multiple workers), use `attach()` to
share the same underlying `SharedCacheData` pointer.

For integration tests of the ReadHandler, push test data directly to the
shared cache via `cache_.push()` before calling `handle()`.  For poll-loop
tests that need deferred data arrival, use a separate thread that calls
`cache_.push()` after a short delay while the handler is blocked in
`wait_for_new_data()`.

For knxd-dependent tests, the `MockKnxdClient` in `mocks/mock_knxd_socket.h`
provides a fake implementation.  It is used by write-handler tests and
connection tests, but is no longer needed by ReadHandler tests (the ReadHandler
uses the shared cache directly, not the knxd socket).

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
