# stallwatch

[![ci](https://github.com/lozlrc/stallwatch/actions/workflows/ci.yml/badge.svg)](https://github.com/lozlrc/stallwatch/actions/workflows/ci.yml)

Low-overhead stall detector for event-loop processes. Instrumented processes
write a heartbeat into a per-process shared-memory slot on every loop
iteration; a monitor daemon scans the slots, flags any heartbeat older than a
threshold, records stall durations, and can capture a backtrace from the
stalled process at the moment of the stall via a cooperative signal protocol.

Inspired by the stall-diagnosis problem described in HRT's 2025 SWE intern
spotlight (https://www.hudsonrivertrading.com/hrtbeat/intern-spotlight-2025-software-engineering-summer-projects/);
this is an independent implementation with no affiliation. The original used
non-cooperative remote unwinding and Intel PT. This project implements both
capture modes: a cooperative SIGUSR2 protocol, and a non-cooperative ptrace
plus libunwind path for targets that cannot run a handler. Intel PT is
documented as a not-implemented extension. The monitor was developed on macOS
arm64; the Linux paths, including remote capture, are built and tested in a
Debian container (`linux/run_linux_tests.sh`).

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

On Linux, `--capture-mode remote` captures a stalled process's stack with
ptrace plus libunwind and needs no handler in the target;
`linux/run_linux_tests.sh` builds with g++ and runs the whole suite, remote
capture included, in a container (it needs `--cap-add=SYS_PTRACE`, which the
script passes).

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
stalling frame. `backtrace()` is not formally async-signal-safe (it is not on
the POSIX list, and glibc may load a helper library on first use); Session
warms it up once at construction outside signal context, and the residual
risk is accepted plainly here because this is a diagnostic tool, not a
correctness dependency. Handler time measured on this machine: p50 0.6 us,
p99 3.1 us across 200 captures.

One portability finding worth stating: on macOS `backtrace()` crosses the
signal frame into the interrupted code, so the cooperative capture symbolizes
the demo's `stall_here` directly. On glibc/aarch64 it does not reliably cross
the signal frame; it stops in libc, so the user frame is missing. That is one
reason the remote path below exists. The signal integration test therefore
asserts the `stall_here` symbol only on macOS, and the remote test asserts it
on Linux.

## Non-temporal stores

`beat()` uses a plain relaxed atomic store by default; building with
`-DSW_NT=1` switches the timestamp store to a non-temporal one
(`_mm_stream_si64` on x86_64, `__builtin_nontemporal_store` under clang
elsewhere, and a plain store where neither exists, such as gcc on aarch64).
Both variants are also callable explicitly (`beat_plain` / `beat_nt`), which
is what the bench and `sw_demo --nt` use.

Measured on this M2 Pro: the NT variant is slightly slower (10.84 vs
10.17 ns/beat) and has no effect on the cache-interference probe (4 MB scan
throughput 90.4 vs 90.7 GB/s, within noise). Two reasons this is expected
here. First, the beat touches one cacheline at a low rate, so there is
nothing for a streaming hint to save. Second, Apple clang lowers an 8-byte
`__builtin_nontemporal_store` on arm64 to `stnp w, w` (a non-temporal pair of
32-bit halves), which adds a shift and, notably, gives up single-copy
atomicity for the 8-byte value; the seq rule keeps a torn read from producing
a false stall, but there is no upside to pay for it.

The x86-64 answer is worse, and it is the interesting one. Measured in CI on
GitHub runners, `movnti` costs about 16 times a plain store: 386.9 vs 23.7
ns/beat under clang, 367.6 vs 30.6 under g++. A non-temporal store is built
for streaming data you will not read again, so it bypasses the cache and
leaves the line in a write-combining buffer. The beat does the opposite: it
rewrites one hot cacheline over and over, so every NT store forces a partial
line out to memory instead of being absorbed by the cache, and the next store
starts over. The cache is exactly what you want here.

Conclusion on both architectures: use the default plain store. The NT path
stays in because the measurement is the point, and because the honest answer
is not the one the idea suggests.

## Remote capture (ptrace + libunwind)

`--capture-mode remote` captures a backtrace with no cooperation from the
target, for stalls the signal path cannot reach: SIGUSR2 masked, the thread
stopped, or simply a fuller stack than a signal handler produces (on
glibc/aarch64 the cooperative handler cannot cross its own signal frame, see
above). Linux only; the monitor refuses the mode on other platforms, and the
build links libunwind only when `SW_REMOTE=1` (the default on Linux).

On stall onset the monitor:

1. `ptrace(PTRACE_SEIZE, pid)`, then `PTRACE_INTERRUPT` to group-stop the
   target without injecting a signal.
2. `waitpid` for the stop. If it raced with a real signal-delivery stop, that
   signal is re-delivered on detach so nothing is swallowed.
3. Walks the remote stack with libunwind's ptrace accessors
   (`unw_create_addr_space(&_UPT_accessors, 0)`, `_UPT_create(pid)`,
   `unw_init_remote`), which read the target's registers and memory for it.
4. `PTRACE_DETACH`, letting the target run again.

Where the target is stopped decides how far the walk gets. The demo's first
stall loop read the clock every iteration, so the target was almost always
inside the vDSO's `clock_gettime` when the monitor interrupted it. On x86-64
libunwind stepped out of the vDSO and reached the stalling function; on
aarch64 it stopped there, returning two frames in libc that no exe-relative
symbolizer can resolve. The demo now samples the clock once per batch of work,
which keeps the instruction pointer in its own code, where a stalled event
loop would really be. The Makefile also builds it with
`-fasynchronous-unwind-tables -funwind-tables` rather than trusting a compiler
default, and the container harness runs the suite under both gcc and clang so
this class of difference cannot hide again.

The target is frozen only between the interrupt and the detach; in remote mode
the report's `handler_us` field carries that frozen window. This path crosses
the signal frame that defeats glibc `backtrace()`, so the remote integration
test asserts `stall_here` in the symbolized frames on Linux. Measured frozen
window to unwind a stalled process from outside: p50 323 us (see benchmarks).

Not implemented: Intel PT. `perf_event_open` with the intel_pt PMU records a
continuous branch trace, so decoding the window around a stall shows where the
time went rather than a single snapshot. It is x86-only and out of scope for
this arm64-developed repo.

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

### Linux (container)

Built with g++ 12 in a Debian container via `linux/run_linux_tests.sh`; raw
output at `bench/results_linux.txt`. Beat cost on Linux aarch64:

```
empty_loop        0.30 ns/iter
clock_read       19.17 ns/iter
beat_plain       19.45 ns/iter
beat_nt          19.43 ns/iter
```

`CLOCK_MONOTONIC_RAW` reads slower here than macOS `CLOCK_UPTIME_RAW`, and NT
is neutral as on macOS. Remote capture (50 injected 2 ms stalls, threshold
500 us, poll 100 us):

```
frozen_window_us    p50=322.7  p99=533.7  max=618.2
detect_latency_us   p50=107.6  p99=232.2
```

The frozen window is how long ptrace pauses the target for the stack walk. The
container runs in a VM on the same M2 Pro, so read the Linux numbers as
order-of-magnitude, not a head-to-head with the native macOS run.

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

- `tests/test_integration_remote.sh`: the same scenario with
  `--capture-mode remote`, so the client never runs a handler. Asserts 3
  stalls, a non-zero frozen window per stall, frames captured, and
  `stall_here` in the symbolized output. Linux only; self-skips on macOS and
  when the monitor was built without `SW_REMOTE`.

`sw_demo` is built at -O0 with frame pointers so the captured backtrace
reliably contains the noinline `stall_here` frame.

`linux/run_linux_tests.sh` runs the full suite (unit, signal integration, and
remote integration) under g++ in a Debian container and refreshes
`bench/results_linux.txt`. It copies the source into the container, so host
build artifacts are never touched, and passes `--cap-add=SYS_PTRACE` for the
remote path.

## Limitations

- Two capture modes with different reach. The cooperative signal mode works
  everywhere but produces no frames if the target masks SIGUSR2, is stopped,
  or is wedged in the kernel, and cannot cross its signal frame on
  glibc/aarch64. The remote mode fixes both but is Linux only and needs
  permission to ptrace a non-descendant: the monitor is a sibling of its
  targets, so yama `ptrace_scope=1` (the default on many distros and on GitHub
  runners) refuses the seize with EPERM. Run the monitor as root, grant it
  `CAP_SYS_PTRACE`, or set `kernel.yama.ptrace_scope=0`; the container harness
  and CI do the equivalent, and the remote test skips loudly where none holds.
  It captures the target's main thread. Intel PT is not implemented.
- One `Session` per process (the signal handler uses process globals), 64
  slots per registry, single machine only.
- Stall durations are beat-to-beat, so resolution is the client's beat
  interval; stalls still in progress when the monitor exits are not recorded.
- Native-hardware numbers are from the macOS arm64 machine. The Linux paths
  (`CLOCK_MONOTONIC_RAW`, `/proc/self/maps` base, `SYS_gettid`, addr2line,
  ptrace remote capture) are built and tested in a container, but those Linux
  numbers come from a VM on the same host, not bare metal. glibc is assumed
  for `execinfo.h` and libunwind for remote capture (musl and a no-libunwind
  build compile the cooperative path only).
- `backtrace()` from a signal handler is not formally async-signal-safe; see
  the capture section.
