#!/usr/bin/env bash
# Remote-capture bench (Linux only): 50 injected 2 ms stalls captured with
# --capture-mode remote. Reports p50/p99 of the frozen window (handler_us,
# interrupt to detach) and of detect latency. Run via linux/run_linux_tests.sh.
set -u

if [ "$(uname -s)" != "Linux" ]; then
  echo "bench-remote: SKIP (Linux only)"
  exit 0
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/bin"
RPT="${TMPDIR:-/tmp}/sw_bench_remote.$$.txt"
export STALLWATCH_SHM="/swbr$$"

"$BIN/stallwatchd" --threshold-us 500 --poll-us 100 --capture-mode remote \
  --max-stalls 50 --run-for-ms 15000 --report "$RPT" --unlink-on-exit 2>/dev/null &
MON=$!
sleep 0.3
"$BIN/sw_demo" --stall-ms 2 --stall-every-ms 40 --stalls 50 --run-for-ms 8000 2>/dev/null
wait "$MON"

n=$(grep -c '^stall ' "$RPT")
echo "remote capture bench: $n stalls of 2 ms, threshold 500 us, poll 100 us"
awk '
  function pct(arr, count, p,   idx) { idx = int(count * p); if (idx < 1) idx = 1; return arr[idx] }
  /^stall / {
    for (i = 1; i <= NF; i++) {
      if ($i ~ /^handler_us=/)     { sub(/^handler_us=/, "", $i); h[++nh] = $i + 0 }
      else if ($i ~ /^detect_us=/) { sub(/^detect_us=/, "", $i); d[++nd] = $i + 0 }
    }
  }
  END {
    asort_h = asort_d = 0
    # portable sort (mawk has no asort)
    for (i = 1; i <= nh; i++) for (j = i + 1; j <= nh; j++) if (h[j] < h[i]) { t = h[i]; h[i] = h[j]; h[j] = t }
    for (i = 1; i <= nd; i++) for (j = i + 1; j <= nd; j++) if (d[j] < d[i]) { t = d[i]; d[i] = d[j]; d[j] = t }
    printf "frozen_window_us    p50=%.1f  p99=%.1f  max=%.1f\n", pct(h, nh, 0.50), pct(h, nh, 0.99), h[nh]
    printf "detect_latency_us   p50=%.1f  p99=%.1f\n", pct(d, nd, 0.50), pct(d, nd, 0.99)
  }
' "$RPT"
rm -f "$RPT"
