# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed

- **Cache reader burned 100% CPU when knxd was missing.**
  `KnxdClient::wait_for_activity()` reported "not connected" and
  `POLLHUP`/`POLLERR`/`POLLNVAL` as a plain timeout, so the cache reader
  loop retried `poll()` in a tight userspace loop and never reconnected —
  `/r` kept returning stale values forever while one CPU core was saturated.
  `WaitResult` now has an `Error` state and the loop (new
  `run_cache_reader()` in `src/knxd/cache_reader.cpp`) reconnects forever
  with an exponential backoff (500 ms … 30 s, rate-limited logging).
- The daemon no longer refuses to start (or exits) when knxd is unavailable:
  parent, cache reader and workers log a warning and retry in the background,
  so knxd may be restarted at any time without restarting the backend.
- `KnxdClient::connect()` now remembers the socket path even when the attempt
  fails, so `reconnect()` can recover once knxd is back.

## [1.0.0] — 2026-07-05

### Added

- Initial release of cometvisu-knxd-fcgi.
- FastCGI backend implementing the CometVisu Protocol.
- `/l` (login) endpoint with session management.
- `/r` (read) endpoint with long-poll support via `poll()`.
- `/w` (write) endpoint for sending KNX telegrams.
- knxd Unix socket client with eibd binary protocol support.
- Address cache integration with knxd's built-in cache.
- Configurable knxd socket path (`KNXD_SOCKET` env var).
- Configurable long-poll timeout (`LONGPOLL_TIMEOUT_SEC` env var).
- Debug logging via `DEBUG_BACKEND` environment variable.
- JSON response builder (no external JSON library dependency).
- Query string parser.
- Comprehensive test suite: unit, integration, and end-to-end tests.
- CMake build system with Google Test integration.
- GPLv3 licensed.
