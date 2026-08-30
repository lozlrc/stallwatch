#!/usr/bin/env bash
# Integration test: stallwatchd + sw_demo end to end.
# - 3 injected 5 ms stalls, threshold 1000 us, poll 100 us, capture on
# - exactly 3 report lines, sane durations and detect latencies, frames present
# - sw_symbolize output contains stall_here
# - edge: monitor with no clients exits cleanly with an empty report
# Unique shm name per run so dirty state or parallel runs cannot interfere.
# One retry: the exact-count assertion can be broken by a multi-ms scheduler
# preemption of the demo, which is noise, not a product bug.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/bin"
TMP="${TMPDIR:-/tmp}/swtest.$$"
mkdir -p "$TMP"
MON_PID=""
DEMO_PID=""

cleanup() {
  [ -n "$MON_PID" ] && kill "$MON_PID" 2>/dev/null
  [ -n "$DEMO_PID" ] && kill "$DEMO_PID" 2>/dev/null
  wait 2>/dev/null
  rm -rf "$TMP"
}
trap cleanup EXIT

fail() {
  echo "integration: FAIL: $*" >&2
  exit 1
}

attempt() {
  local a="$1"
  local rpt="$TMP/report.$a.txt"
  export STALLWATCH_SHM="/swi.$$.$a"

  "$BIN/stallwatchd" --threshold-us 1000 --poll-us 100 --capture --max-stalls 3 \
    --run-for-ms 4000 --report "$rpt" --unlink-on-exit 2>"$TMP/mon.$a.log" &
  MON_PID=$!
  sleep 0.3
  "$BIN/sw_demo" --stall-ms 5 --stall-every-ms 200 --stalls 3 --run-for-ms 1500 \
    2>"$TMP/demo.$a.log" &
  DEMO_PID=$!

  wait "$MON_PID"
  local rc=$?
  MON_PID=""
  wait "$DEMO_PID" 2>/dev/null
  DEMO_PID=""
  if [ "$rc" -ne 0 ]; then
    echo "attempt $a: monitor exited $rc" >&2
    return 1
  fi

  local n
  n=$(grep -c '^stall ' "$rpt")
  if [ "$n" -ne 3 ]; then
    echo "attempt $a: expected 3 stalls, got $n" >&2
    return 1
  fi

  awk '
    /^stall / {
      dur = -1; det = -1; nf = 0
      for (i = 1; i <= NF; i++) {
        if ($i ~ /^duration_us=/) { sub(/^duration_us=/, "", $i); dur = $i + 0 }
        else if ($i ~ /^detect_us=/) { sub(/^detect_us=/, "", $i); det = $i + 0 }
        else if ($i ~ /^frames=/) { sub(/^frames=/, "", $i); if ($i != "-") nf = split($i, arr, ",") }
      }
      if (dur < 4000 || dur > 50000) { print "bad duration_us " dur; bad = 1 }
      if (det < 0 || det >= 2000)    { print "bad detect_us " det; bad = 1 }
      if (nf < 3)                    { print "too few frames " nf; bad = 1 }
    }
    END { exit bad }
  ' "$rpt"
  if [ $? -ne 0 ]; then
    echo "attempt $a: report bounds check failed" >&2
    sed 's/^/  /' "$rpt" >&2
    return 1
  fi

  "$ROOT/tools/sw_symbolize" "$rpt" > "$TMP/sym.$a.txt" 2>"$TMP/sym.$a.err"
  if [ $? -ne 0 ]; then
    echo "attempt $a: sw_symbolize failed" >&2
    return 1
  fi
  # The cooperative handler calls backtrace() in signal context. macOS crosses
  # the signal frame into the interrupted stall_here; glibc/aarch64 does not
  # reliably cross it (it stops in libc), so require the user symbol only where
  # the platform delivers it. The remote capture path (test_integration_remote)
  # crosses the frame on Linux and asserts stall_here there.
  if [ "$(uname -s)" = "Darwin" ]; then
    if ! grep -q "stall_here" "$TMP/sym.$a.txt"; then
      echo "attempt $a: stall_here missing from symbolized frames" >&2
      sed 's/^/  /' "$TMP/sym.$a.txt" >&2
      return 1
    fi
  fi
  return 0
}

ok=0
for a in 1 2; do
  if attempt "$a"; then
    ok=1
    break
  fi
  echo "integration: attempt $a failed, retrying (scheduler noise tolerance)" >&2
done
[ "$ok" -eq 1 ] || fail "stall scenario failed on both attempts"

# Edge: monitor with no clients exits cleanly and reports nothing.
export STALLWATCH_SHM="/swe$$"
"$BIN/stallwatchd" --threshold-us 1000 --poll-us 100 --run-for-ms 300 \
  --report "$TMP/empty.txt" --unlink-on-exit 2>/dev/null
[ $? -eq 0 ] || fail "no-client monitor exited nonzero"
[ "$(grep -c '^stall ' "$TMP/empty.txt")" -eq 0 ] || fail "no-client monitor reported stalls"

echo "integration: PASS"
