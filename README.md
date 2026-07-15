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

# Run via spawn-fcgi (traditional FastCGI mode)
spawn-fcgi -p 9000 -n ./build/src/cometvisu-knxd-fcgi

# Run standalone (direct socket — no spawn-fcgi needed)
FCGI_SOCKET=:9000 ./build/src/cometvisu-knxd-fcgi

# Run standalone on a Unix socket
FCGI_SOCKET=/tmp/cometvisu-fcgi.sock ./build/src/cometvisu-knxd-fcgi
```

### Environment variables

| Variable              | Default    | Description                              |
|-----------------------|------------|------------------------------------------|
| `KNXD_SOCKET`         | `/run/knx` | Path to the knxd Unix socket             |
| `FCGI_SOCKET`         | *(unset)*  | Direct FCGI socket (`:port` for TCP, or filesystem path for Unix socket). When set, the server runs standalone without `spawn-fcgi`. |
| `FCGI_WORKERS`        | `20`       | Number of worker processes in direct socket mode (1–256). Each worker handles one concurrent client using `fork()`-based pre-forking. Idle workers cost ~few MB resident RAM each. |
| `LONGPOLL_TIMEOUT_SEC`| `300`      | Max seconds to wait in long-poll `/r`    |
| `CGI_URL_PATH`        | *(unset)*  | When set, the login (`/l`) response includes a config object `"c"` with `"baseURL"` set to this value. Used by the web server to advertise the URL prefix under which the backend is reachable (e.g. `/proxy/visu`). |
| `DEBUG_BACKEND`       | *(unset)*  | Set to `1` to enable debug logging to stderr |

### FastCGI environment variables

These are standard
[CGI variables](https://fastcgi-archives.github.io/FastCGI_Specification.html)
provided by the web server. The application reads them automatically — you do
not need to set them yourself.

| Variable          | Description                                        |
|-------------------|----------------------------------------------------|
| `REQUEST_METHOD`  | HTTP method: `GET`, `POST`, or `PUT`               |
| `REQUEST_URI`     | Full request URI, e.g. `/cgi-bin/l?u=USER&p=PASS`  |
| `QUERY_STRING`    | Query string portion, e.g. `u=USER&p=PASS&d=DEVICE`|
| `SCRIPT_NAME`     | Virtual path to the script, e.g. `/cgi-bin/l`     |
| `PATH_INFO`       | Extra path after the script name, e.g. `/l`       |
| `CONTENT_TYPE`    | MIME type of the request body (POST/PUT)           |
| `CONTENT_LENGTH`  | Size of the request body in bytes (POST/PUT)       |
| `SERVER_PROTOCOL` | Protocol string, e.g. `HTTP/1.1`                   |

> **Routing note**: The application determines which handler (`/l`, `/r`,
> `/w`) to invoke by inspecting `PATH_INFO` first. If `PATH_INFO` is empty,
> it strips `SCRIPT_NAME` from `REQUEST_URI` to derive the path. When
> `SCRIPT_NAME` itself ends with the endpoint (e.g. `/cgi-bin/l`), the last
> path component is used as the handler key. This means all common web-server
> configurations — `ScriptAlias`, `fastcgi_pass` with `SCRIPT_NAME` pointing
> to a directory or directly to the binary — work out of the box.

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
