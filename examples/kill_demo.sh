#!/usr/bin/env bash
# The 60-second demo: a publisher, a hot standby and a subscriber share a channel. The publisher
# is killed with SIGKILL; the standby takes over and the subscriber keeps receiving, with one
# epoch change and no gap or duplicate.
#
# Usage: examples/kill_demo.sh [build-dir]   (default: build/release)
set -euo pipefail

build="${1:-build/release}"
channel="kill_demo_$$"
log="$(mktemp)"
pids=()

cleanup() {
  kill -9 "${pids[@]}" 2>/dev/null || true
  wait 2>/dev/null || true
  rm -f "/dev/shm/revenant.${channel}" "$log"
}
trap cleanup EXIT

"$build/examples/ticker_publisher" --channel="$channel" --rate=20000 &
publisher=$!
pids+=("$publisher")
sleep 0.5
"$build/examples/ticker_publisher" --channel="$channel" --rate=20000 --standby &
pids+=($!)
"$build/examples/ticker_subscriber" --channel="$channel" --seconds=4 >"$log" &
subscriber=$!
pids+=("$subscriber")

sleep 1.5
"$build/revenant-stat" "$channel"
echo ">>> kill -9 the publisher (pid $publisher)"
kill -9 "$publisher"
sleep 1
"$build/revenant-stat" "$channel"

wait "$subscriber"
cat "$log"

summary="$(grep '^summary:' "$log")"
grep -q 'epoch_changes=1 ' <<<"$summary" || { echo "FAIL: expected exactly one takeover"; exit 1; }
grep -q 'gaps=0 ' <<<"$summary" || { echo "FAIL: messages were lost"; exit 1; }
grep -q 'duplicates=0$' <<<"$summary" || { echo "FAIL: a message was delivered twice"; exit 1; }
echo "OK: the standby took over and the stream continued with no gap and no duplicate"
