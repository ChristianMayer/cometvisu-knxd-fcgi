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

## Architecture

See [PLAN.md](PLAN.md) for the full architecture and design document.

## License

See [LICENSE](LICENSE).
