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
- **A `/w` request can no longer block forever on the knxd handshake.**
  `open_group_socket()` read the `EIB_OPEN_GROUPCON` reply from a blocking
  socket without a deadline, so a knxd that accepted the connection but never
  answered hung the worker (and with it the request) indefinitely.  The reply
  is now read with a deadline (default 5 s, `set_handshake_timeout_ms()`).
- **Writes and logins are no longer rejected with 503.** The concurrency and
  load-shedding semaphores applied to `/w`/`/l` as well, so a pool full of
  long-polls made switch operations fail.  Both now only guard `/r`.
  Tokens are released via RAII (also when a handler throws — that used to
  terminate the worker process; it now returns HTTP 500), and a worker that
  dies restores its token instead of permanently shrinking the pool.
- **The parent supervisor restarts dead children.** The SIGCHLD handler used
  to `waitpid()` all children itself, so the supervisor never saw an exit
  status: a crashed worker was never replaced and a dead cache reader was
  never restarted (leaving `/r` on stale data forever).  The parent is now the
  only reaper, keeps the listen socket open for replacements, restarts workers
  and the cache reader with a crash-loop backoff, and no longer ignores
  SIGTERM while blocked in `wait()` (`sigaction()` without `SA_RESTART`).
- `FcgiRequest::path()` falls back to the last component of `SCRIPT_NAME` when
  no `REQUEST_URI` is passed (pure-CGI setups such as `cgi-fcgi`).

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
