#!/usr/bin/env bash
#
# start-knxd-ipt.sh — Start knxd with a virtual KNX bus via IP Routing.
#
# knxd uses the `ip:` layer-2 driver with a multicast address that is unlikely
# to exist on the network (239.255.255.250). This lets knxd start without any
# physical KNX hardware while still providing:
#
#   - A Unix socket for the FCGI app to connect to
#   - Group cache (knxd -c) — stores telegrams seen on the bus
#   - Full eibd protocol support
#
# Since there is no actual KNX bus, bus-dependent operations behave as
# expected:
#   - Writes are sent to the multicast address (no-op on the network)
#   - Cache reads return empty (404) unless data was previously cached
#   - Long-poll waits for telegrams that never arrive (returns empty after
#     the configured timeout)
#
# For testing with actual bus data, use a real KNX IP interface by setting
# the KNXD_INTERFACE env var (e.g., KNXD_INTERFACE=iptn:192.168.0.10:3670).
#
# Used by VS Code tasks to provide knxd for development and E2E testing.
#
# The script:
#   1. Starts knxd in the background.
#   2. Waits for the Unix socket to appear (up to 15 seconds).
#   3. Prints "knxd ready on <socket>" once knxd is accepting connections.
#   4. Stays alive until knxd exits (or is killed).
#
# Configuration (via environment variables):
#   KNXD_EIBADDR    knxd's own physical address  (default: 1.1.220)
#   KNXD_CLIENT     client address range          (default: 1.1.221:3)
#   KNXD_SOCKET     Unix socket path              (default: /tmp/knxd-ipt)
#   KNXD_INTERFACE  KNX bus interface URL         (default: ip:239.255.255.250)

set -uo pipefail

EIBADDR="${KNXD_EIBADDR:-1.1.220}"
CLIENT="${KNXD_CLIENT:-1.1.221:3}"
SOCKET="${KNXD_SOCKET:-/tmp/knxd-ipt}"
INTERFACE="${KNXD_INTERFACE:-ip:239.255.255.250}"

echo "[start-knxd] Starting knxd with:"
echo "  EIB address:  $EIBADDR"
echo "  Client addrs: $CLIENT"
echo "  Socket:       $SOCKET"
echo "  Interface:    $INTERFACE"

# Clean up any stale socket
if [ -e "$SOCKET" ]; then
  echo "[start-knxd] Removing stale socket $SOCKET"
  rm -f "$SOCKET" 2>/dev/null || echo "[start-knxd] Warning: could not remove $SOCKET"
fi

# Start knxd in the background
#   -e, -E, -u  = global options
#   -c          = enable group cache
#   -D -T -R -S = EIBnet/IP server capabilities
#   -f 9        = max error level
#   -b <iface>  = layer-2 driver (IP Routing to an unlikely multicast address)
knxd \
  -e "$EIBADDR" \
  -E "$CLIENT" \
  -u "$SOCKET" \
  -c100 \
  -D -T -R -S \
  -f 9 \
  -b "$INTERFACE" \
  &>/tmp/knxd-ipt.log &
KNXD_PID=$!

echo "[start-knxd] knxd PID: $KNXD_PID (log: /tmp/knxd-ipt.log)"

# Wait for the socket to appear (up to 15 seconds)
WAIT_MAX=15
for i in $(seq 1 "$WAIT_MAX"); do
  if [ -S "$SOCKET" ]; then
    echo "[start-knxd] knxd ready on $SOCKET"
    break
  fi
  if ! kill -0 "$KNXD_PID" 2>/dev/null; then
    echo "[start-knxd] ERROR: knxd died unexpectedly!"
    cat /tmp/knxd-ipt.log
    exit 1
  fi
  sleep 1
done

if ! [ -S "$SOCKET" ]; then
  echo "[start-knxd] ERROR: knxd did not create $SOCKET within ${WAIT_MAX}s"
  cat /tmp/knxd-ipt.log
  exit 1
fi

# Keep running until knxd exits
wait "$KNXD_PID"
