#!/usr/bin/env bash
# bench_detect: detection latency, measured-duration error, and capture pause.
# 200 injected 2 ms stalls, threshold 500 us, poll 100 us, capture on.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/bin"
RPT="$ROOT/bench/detect_report.txt"
export STALLWATCH_SHM="/swbd.$$"

STALLS=200
STALL_MS=2
EVERY_MS=20

echo "== bench_detect"
echo "config: stalls=$STALLS stall_ms=$STALL_MS every_ms=$EVERY_MS threshold_us=500 poll_us=100 capture=on"

"$BIN/stallwatchd" --threshold-us 500 --poll-us 100 --capture --max-stalls "$STALLS" \
  --run-for-ms 15000 --report "$RPT" --unlink-on-exit 2>/dev/null &
MON=$!
sleep 0.3
"$BIN/sw_demo" --stall-ms "$STALL_MS" --stall-every-ms "$EVERY_MS" --stalls "$STALLS" \
  --run-for-ms 8000 2>/dev/null &
DEMO=$!
trap 'kill $MON $DEMO 2>/dev/null; wait 2>/dev/null' EXIT

wait "$MON"
rc=$?
wait "$DEMO" 2>/dev/null
trap - EXIT
if [ "$rc" -ne 0 ]; then
  echo "bench_detect: monitor exited $rc" >&2
  exit 1
fi

n=$(grep -c '^stall ' "$RPT")
echo "stalls recorded: $n"

# Percentile over one extracted column (nearest-rank).
pctl() {
  sort -n | awk -v p="$1" '{ a[NR] = $1 } END {
    if (NR == 0) { print "nan"; exit }
    i = int((p / 100) * NR + 0.999999); if (i < 1) i = 1; if (i > NR) i = NR
    printf "%.1f", a[i]
  }'
}

col() {
  awk -v key="$1" '/^stall / {
    for (i = 1; i <= NF; i++)
      if (index($i, key "=") == 1) { sub("^" key "=", "", $i); print $i + 0 }
  }' "$RPT"
}

DET50=$(col detect_us | pctl 50);   DET99=$(col detect_us | pctl 99)
ERR50=$(col duration_us | awk -v t=$((STALL_MS * 1000)) '{ print $1 - t }' | pctl 50)
ERR99=$(col duration_us | awk -v t=$((STALL_MS * 1000)) '{ print $1 - t }' | pctl 99)
HND50=$(col handler_us | pctl 50);  HND99=$(col handler_us | pctl 99)

echo "detect_latency_us   p50=$DET50  p99=$DET99"
echo "duration_error_us   p50=$ERR50  p99=$ERR99  (measured duration minus injected ${STALL_MS}000 us)"
echo "capture_handler_us  p50=$HND50  p99=$HND99  (time inside the SIGUSR2 backtrace handler)"
