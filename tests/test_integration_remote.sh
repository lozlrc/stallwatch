#!/usr/bin/env bash
# Integration test for the non-cooperative capture path (ptrace + libunwind).
# Linux only; self-skips elsewhere and when stallwatchd was built without
# SW_REMOTE. Same scenario as the signal test but --capture-mode remote, so
# the client never runs a handler: the monitor freezes it and unwinds its
# stack from outside. Inside docker this needs --cap-add=SYS_PTRACE.
set -u

if [ "$(uname -s)" != "Linux" ]; then
  echo "integration-remote: SKIP (Linux only)"
  exit 0
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/bin"
TMP="${TMPDIR:-/tmp}/swremote.$$"
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
  echo "integration-remote: FAIL: $*" >&2
  exit 1
}

# Probe: a build without SW_REMOTE refuses the mode with exit code 2.
export STALLWATCH_SHM="/swrp$$"
"$BIN/stallwatchd" --capture-mode remote --run-for-ms 1 --report "$TMP/probe.txt" \
  --unlink-on-exit 2>/dev/null
rc=$?
if [ "$rc" -eq 2 ]; then
  echo "integration-remote: SKIP (built without SW_REMOTE)"
  exit 0
fi
[ "$rc" -eq 0 ] || fail "probe run exited $rc"

attempt() {
  local a="$1"
  local rpt="$TMP/report.$a.txt"
  export STALLWATCH_SHM="/swr.$$.$a"

  "$BIN/stallwatchd" --threshold-us 1000 --poll-us 100 --capture-mode remote \
    --max-stalls 3 --run-for-ms 4000 --report "$rpt" --unlink-on-exit \
    2>"$TMP/mon.$a.log" &
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
    cat "$TMP/mon.$a.log" >&2
    return 1
  fi

  awk '
    /^stall / {
      dur = -1; det = -1; hus = -1; nf = 0
      for (i = 1; i <= NF; i++) {
        if ($i ~ /^duration_us=/)      { sub(/^duration_us=/, "", $i); dur = $i + 0 }
        else if ($i ~ /^detect_us=/)   { sub(/^detect_us=/, "", $i); det = $i + 0 }
        else if ($i ~ /^handler_us=/)  { sub(/^handler_us=/, "", $i); hus = $i + 0 }
        else if ($i ~ /^frames=/)      { sub(/^frames=/, "", $i); if ($i != "-") nf = split($i, arr, ",") }
      }
      if (dur < 4000 || dur > 60000) { print "bad duration_us " dur; bad = 1 }
      if (det < 0 || det >= 2000)    { print "bad detect_us " det; bad = 1 }
      if (hus <= 0)                  { print "missing frozen-window handler_us"; bad = 1 }
      if (nf < 2)                    { print "too few frames " nf; bad = 1 }
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
  if ! grep -q "stall_here" "$TMP/sym.$a.txt"; then
    echo "attempt $a: stall_here missing from symbolized frames" >&2
    sed 's/^/  /' "$TMP/sym.$a.txt" >&2
    return 1
  fi
  return 0
}

ok=0
for a in 1 2; do
  if attempt "$a"; then
    ok=1
    break
  fi
  echo "integration-remote: attempt $a failed, retrying (scheduler noise tolerance)" >&2
done
[ "$ok" -eq 1 ] || fail "remote capture scenario failed on both attempts"

echo "integration-remote: PASS"
