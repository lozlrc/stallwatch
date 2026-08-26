# stallwatch

Low-overhead stall detector for event-loop processes. Instrumented processes
write a heartbeat into a per-process shared-memory slot on every loop
iteration; a monitor daemon scans the slots, flags any heartbeat older than a
threshold, records stall durations, and can capture a backtrace from the
stalled process at the moment of the stall via a cooperative signal protocol.

Inspired by the stall-diagnosis problem described in HRT's 2025 SWE intern
spotlight (https://www.hudsonrivertrading.com/hrtbeat/intern-spotlight-2025-software-engineering-summer-projects/);
this is an independent implementation with no affiliation. The original used
non-cooperative remote unwinding and Intel PT; this project implements the
cooperative-signal design and documents the remote-unwind approach as a
sketch below.

## Quick start

```
make            # builds bin/stallwatchd, bin/sw_demo, bin/test_unit, bin/bench_beat
make test       # unit + integration tests
make bench      # writes bench/results.txt (about 15 s)

# by hand:
bin/stallwatchd --threshold-us 1000 --poll-us 100 --capture &
bin/sw_demo --stall-ms 5 --stall-every-ms 200 --stalls 3
tools/sw_symbolize stallwatch_report.txt
```

Instrumenting your own loop:

```cpp
#include <stallwatch/stallwatch.hpp>

sw::Session s("my_loop");   // claims a slot in /stallwatch.<uid>
if (!s.ok()) ...;           // registry full or shm unavailable
for (;;) {
  s.beat();                 // one relaxed 8-byte store per iteration
  do_work();
}
```

`STALLWATCH_SHM` overrides the shm name for both client and monitor (the
tests use this to isolate runs).

## Design

```
 instrumented procs           shared memory               monitor                 tooling

 sw::Session::beat()  --->  /stallwatch.<uid>    <---  stallwatchd  --->  stallwatch_report.txt
 (one relaxed store)        header + 64 slots          polls line A             |
        ^                   (128 B each) + spill       every poll-us            v
        |                                                  |             tools/sw_symbolize
        +------ SIGUSR2 <------ capture request -----------+             (atos / addr2line)
                handler writes frames into the slot's spill area
```

The registry is one `shm_open` segment: a 64-byte header, then 64 slots, then
a per-slot spill area (128 u64 frame addresses plus 256 bytes of exe path).
Each slot is exactly two 64-byte cachelines so a beat dirties one line and
never contends with the cold registration data:

| line     | offset | field                 | written by            |
|----------|--------|-----------------------|-----------------------|
| A (hot)  | 0      | atomic u64 `beat_ns`  | client, every beat    |
| A        | 8      | atomic u64 `seq`      | client, every beat    |
| A        | 16     | u32 `tag`             | client, every beat    |
| A        | 20     | pad to 64             |                       |
| B (cold) | 64     | i32 `pid`             | registration          |
| B        | 72     | u64 `tid`             | registration          |
| B        | 80     | char `name[24]`       | registration          |
| B        | 104    | u64 `aslr_slide`      | registration          |
| B        | 112    | atomic u32 `state`    | claim + capture       |
| B        | 116    | u32 `capture_len`     | capture handler       |
| B        | 120    | u32 `exe_len`         | registration          |
| B        | 124    | u32 `handler_ns`      | capture handler       |

`aslr_slide` holds the main image load address (macOS: the mach header
address from `_dyld_get_image_header(0)`, which is vmaddr base plus slide;
Linux: the exe's first mapping base from `/proc/self/maps`). That is exactly
what `atos -l` and the addr2line offset adjustment need.

Claiming is a CAS of `state` FREE to ACTIVE followed by the line B fill; the
monitor never judges a slot on its first observation and skips `beat_ns == 0`,
so a half-registered slot cannot produce a verdict. Slots are released in the
Session destructor, and the monitor reclaims any slot whose pid is gone
(`kill(pid, 0)` returning ESRCH) every 50 ms, so crashed clients cannot leak
slots.

**Why a beat is one relaxed store.** The beat sits on the hot path of someone
else's event loop, so it must not fence, CAS, or syscall. Same-machine shared
memory needs no ordering for this: the monitor only ever compares values from
the same writer, and eventual visibility (nanoseconds in practice) only adds
to detection latency, which the threshold already dominates. Measured on this
machine, `beat()` costs 10.17 ns and 10.09 ns of that is reading
`CLOCK_UPTIME_RAW`; the store itself is under 0.1 ns of marginal cost.

**The seq rule.** A stall verdict requires both `now - beat_ns > threshold`
and `seq` unchanged since the previous poll. If the client wrote anything
between two polls it is alive, whatever the timestamp says, which makes the
detector immune to timestamp artifacts (a torn non-temporal store, a beat
landing between the monitor's two loads). Clock skew is not a concern since
client and monitor read the same monotonic clock on the same machine, but the
rule costs nothing and closes the class of bugs. Stall duration is measured
beat-to-beat on the client's own clock (last beat before the stall to first
beat after), so monitor scheduling cannot inflate it; resolution is the
client's beat interval.

Detection latency is reported as `t_detect - (last_beat + threshold)`, i.e.
how long after the stall crossed the threshold the monitor noticed. Expected
value is half the poll interval plus scheduling noise; see benchmarks.

## Signal capture protocol

On stall onset with `--capture`:

1. monitor: CAS slot `state` ACTIVE to CAPTURE_REQ
2. monitor: `kill(pid, SIGUSR2)`
3. client handler (installed by Session): if `state == CAPTURE_REQ`, run
   `backtrace()` into the slot's spill area, set `capture_len` and
   `handler_ns`, store `state = CAPTURE_DONE` (release)
4. monitor: poll `state` up to 50 ms, copy frames out, CAS back to ACTIVE
5. on timeout (client wedged harder than a stall, exiting, or signal-masked):
   CAS CAPTURE_REQ back to ACTIVE and report the stall with no frames

The signal interrupts the stalled code, so the captured stack contains the
stalling frame; the demo's `stall_here` shows up symbolized in the
integration test. `backtrace()` is not formally async-signal-safe (it is not
on the POSIX list, and glibc may load a helper library on first use); Session
warms it up once at construction outside signal context, and the residual
risk is accepted plainly here because this is a diagnostic tool, not a
correctness dependency. Handler time measured on this machine: p50 0.6 us,
p99 3.1 us across 200 captures.

## Non-temporal stores

`beat()` uses a plain relaxed atomic store by default; building with
`-DSW_NT=1` switches the timestamp store to a non-temporal one
(`__builtin_nontemporal_store`; on x86_64 `_mm_stream_si64`, which compiles
but is untested here since this machine is arm64). Both variants are also
callable explicitly (`beat_plain` / `beat_nt`), which is what the bench and
`sw_demo --nt` use.

Measured on this M2 Pro: the NT variant is slightly slower (10.84 vs
10.17 ns/beat) and has no effect on the cache-interference probe (4 MB scan
throughput 90.4 vs 90.7 GB/s, within noise). Two reasons this is expected
here. First, the beat touches one cacheline at a low rate, so there is
nothing for a streaming hint to save. Second, Apple clang lowers an 8-byte
`__builtin_nontemporal_store` on arm64 to `stnp w, w` (a non-temporal pair of
32-bit halves), which adds a shift and, notably, gives up single-copy
atomicity for the 8-byte value; the seq rule keeps a torn read from producing
a false stall, but there is no upside to pay for it. Conclusion for this
machine: use the default plain store. The NT path stays in because measuring
it was the point and the x86 story may differ.

## Remote unwind design sketch (not implemented)

The non-cooperative alternative, for targets that cannot run a handler
(signal-masked, wedged in uninterruptible state, or not instrumented):

- On stall onset, attach with `ptrace(PTRACE_SEIZE)` then `PTRACE_INTERRUPT`,
  stopping the target for the duration of the walk.
- Read registers with `PTRACE_GETREGSET` and stack memory with
  `process_vm_readv`, which needs no cooperation from the target.
- Unwind with libunwind-ptrace: `unw_create_addr_space(&_UPT_accessors, 0)`
  plus `_UPT_create(pid)` walks the remote stack using the target's unwind
  tables; detach and let it run again.
- Intel PT goes further: `perf_event_open` with the intel_pt PMU records a
  continuous branch trace, and decoding the window around the stall shows
  where the time went, not just one snapshot.
- All of this is Linux-specific (and PT is x86-specific); macOS offers no
  equivalent of this ptrace surface, which is why this repo ships the
  cooperative protocol instead.

## Benchmarks

Machine: Apple M2 Pro, arm64, Darwin 25.0.0, Apple clang 17.0.0
(clang-1700.6.3.2). All numbers from `make bench` on this machine; raw output
is committed at `bench/results.txt`. Reproduce with `make bench` (about 15 s
total).

Beat cost (200M iterations each):

```
empty_loop        0.44 ns/iter
clock_read       10.09 ns/iter  (ns_now only)
beat_plain       10.17 ns/iter
beat_nt          10.84 ns/iter
```

The beat is the clock read. Cache-interference probe (4 MB summed scan
between beats):

```
scan_4mb_alone    90.7 GB/s
scan_4mb_plain    90.4 GB/s  (one beat_plain per 4 MB scan)
scan_4mb_nt       90.7 GB/s  (one beat_nt per 4 MB scan)
```

Detection quality (200 injected 2 ms spin stalls, threshold 500 us, poll
100 us, capture on, demo beating every 100 us):

```
detect_latency_us   p50=77.2   p99=157.2
duration_error_us   p50=87.5   p99=195.5   (measured duration minus injected 2000 us)
capture_handler_us  p50=0.6    p99=3.1     (time inside the SIGUSR2 backtrace handler)
```

Detect latency lands where the poll interval predicts (under one poll at p50).
Duration error is bounded by the client beat interval (100 us here) plus
scheduler noise, as expected for beat-to-beat measurement.

## Tests

`make test` runs both:

- `tests/test_unit.cpp` (128 checks): slot claim/release, full-registry clean
  failure (65th claim returns -1), dead-pid reclaim against a reaped forked
  child, the detection state machine driven with fake timestamps (onset,
  recovery, exact detect-latency arithmetic, the seq-unchanged rule, the
  never-judge-first-observation rule), report line format round trip
  including an exe path with spaces, and an in-process Session beat check.
- `tests/test_integration.sh`: real `stallwatchd` against real `sw_demo`
  (three 5 ms stalls, threshold 1000 us, poll 100 us, capture on). Asserts
  exactly 3 report lines, each duration in [4 ms, 50 ms], each detect latency
  under 2 ms, at least 3 frames per stall, and that `tools/sw_symbolize`
  output contains `stall_here`. Also asserts a monitor with no clients exits
  cleanly with an empty report. Each run uses a unique `STALLWATCH_SHM` name
  and traps to kill stray processes; one retry absorbs multi-ms scheduler
  preemptions, which the exact-count assertion cannot distinguish from real
  stalls.

`sw_demo` is built at -O0 with frame pointers so the captured backtrace
reliably contains the noinline `stall_here` frame.

## Limitations

- Capture is cooperative only. A target that masks SIGUSR2, is stopped, or is
  wedged in the kernel produces a stall record with no frames (the 50 ms
  handshake times out). The remote-unwind sketch above is the fix and is not
  implemented.
- One `Session` per process (the signal handler uses process globals), 64
  slots per registry, single machine only.
- Stall durations are beat-to-beat, so resolution is the client's beat
  interval; stalls still in progress when the monitor exits are not recorded.
- All numbers above are from this macOS arm64 machine. The Linux code paths
  (`CLOCK_MONOTONIC_RAW`, `/proc/self/maps` base, `SYS_gettid`, addr2line
  symbolization, `-lrt`) are compile-gated and untested here, and glibc is
  assumed for `execinfo.h` (musl lacks it).
- `backtrace()` from a signal handler is not formally async-signal-safe; see
  the capture section.
