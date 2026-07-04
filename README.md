# cometvisu-knxd-fcgi

FastCGI backend in modern C++ that implements the
[CometVisu Protocol](https://github.com/CometVisu/CometVisu/wiki/Protocol)
and connects to a local [knxd](https://github.com/knxd/knxd) daemon.

## Quick Start

```bash
# Prerequisites
sudo apt install libfcgi-dev knxd-dev cmake g++

# Build (using system-installed knxd headers)
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
```

### Building without the knxd-dev system package

If you build knxd from source, point CMake at your checkout:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DKNXD_SOURCE_DIR=/path/to/knxd
cmake --build build -j$(nproc)
```

The build expects to find `eibtypes.h` in `<KNXD_SOURCE_DIR>/include/`.

### Run tests / Run the server

```bash
# Run tests
ctest --test-dir build --output-on-failure

# Run (via spawn-fcgi or your web server's FastCGI support)
spawn-fcgi -p 9000 -n ./build/src/cometvisu-knxd-fcgi
```

### Environment variables

| Variable              | Default    | Description                              |
|-----------------------|------------|------------------------------------------|
| `KNXD_SOCKET`         | `/run/knx` | Path to the knxd Unix socket             |
| `LONGPOLL_TIMEOUT_SEC`| `60`       | Max seconds to wait in long-poll `/r`    |
| `DEBUG_BACKEND`       | *(unset)*  | Set to `1` to enable debug logging to stderr |

### Debug mode

Set `DEBUG_BACKEND=1` (or `true`/`yes`/`on`) to enable detailed debug output to
stderr. This prints every HTTP request/response and all knxd communication with
millisecond timestamps, making it easy to trace the full communication flow:

```
[2026-07-04 12:34:56.789] → HTTP REQUEST: GET /r?a=KNX:1/2/3&t=5
[2026-07-04 12:34:56.790]   → KNXD SEND: cache_read addr=1/2/3 nowait=true
[2026-07-04 12:34:56.791]   ← KNXD RECV: cache_read addr=1/2/3 data=42
[2026-07-04 12:34:56.792] ← HTTP RESPONSE: 200 body={"d":{"KNX:1/2/3":"42"},"i":"1"}
```

KNXD operations are indented under their parent HTTP request. Long URIs (>500
chars) and large response bodies (>1000 chars) are automatically truncated with
a `... (truncated, NNNN chars total)` marker.

## Architecture

See [PLAN.md](PLAN.md) for the full architecture and design document.

## License

See [LICENSE](LICENSE).
