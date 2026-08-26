// sw_demo: an instrumented toy event loop with injectable stalls.
// Built at -O0 with frame pointers so a captured backtrace reliably contains
// stall_here (see Makefile).
#include "stallwatch/stallwatch.hpp"

#include <cstdio>
#include <cstring>

namespace {

inline void cpu_relax() {
#if defined(__aarch64__)
  asm volatile("yield");
#elif defined(__x86_64__)
  __builtin_ia32_pause();
#else
  asm volatile("" ::: "memory");
#endif
}

struct Options {
  uint64_t stall_ms = 0;        // injected stall length
  uint64_t stall_every_ms = 200; // schedule period
  long stalls = 1;              // how many to inject
  uint64_t beat_us = 100;       // beat pacing
  long run_for_ms = -1;         // -1 = derive from schedule
  bool nt = false;              // use the non-temporal store path
  bool sleep_pace = false;      // usleep pacing instead of spin pacing
  const char* name = "sw_demo";
};

void usage(const char* argv0) {
  fprintf(stderr,
          "usage: %s [--stall-ms N] [--stall-every-ms N] [--stalls N] [--beat-us N]\n"
          "          [--run-for-ms N] [--nt] [--sleep] [--name S]\n",
          argv0);
}

bool parse_args(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; i++) {
    auto need = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    const char* v;
    if (!strcmp(argv[i], "--stall-ms")) {
      if (!(v = need())) return false;
      o.stall_ms = strtoull(v, nullptr, 10);
    } else if (!strcmp(argv[i], "--stall-every-ms")) {
      if (!(v = need())) return false;
      o.stall_every_ms = strtoull(v, nullptr, 10);
    } else if (!strcmp(argv[i], "--stalls")) {
      if (!(v = need())) return false;
      o.stalls = strtol(v, nullptr, 10);
    } else if (!strcmp(argv[i], "--beat-us")) {
      if (!(v = need())) return false;
      o.beat_us = strtoull(v, nullptr, 10);
    } else if (!strcmp(argv[i], "--run-for-ms")) {
      if (!(v = need())) return false;
      o.run_for_ms = strtol(v, nullptr, 10);
    } else if (!strcmp(argv[i], "--nt")) {
      o.nt = true;
    } else if (!strcmp(argv[i], "--sleep")) {
      o.sleep_pace = true;
    } else if (!strcmp(argv[i], "--name")) {
      if (!(v = need())) return false;
      o.name = v;
    } else {
      usage(argv[0]);
      return false;
    }
  }
  return true;
}

} // namespace

// The injected stall. Spin, not sleep, so heartbeat timestamps stay on one
// clock and SIGUSR2 lands mid-spin with this frame on the stack. extern "C"
// keeps the symbol unmangled for the symbolizer test.
extern "C" __attribute__((noinline)) void stall_here(uint64_t spin_ns) {
  uint64_t end = sw::ns_now() + spin_ns;
  while (sw::ns_now() < end) cpu_relax();
}

int main(int argc, char** argv) {
  Options o;
  if (!parse_args(argc, argv, o)) return 2;
  if (o.run_for_ms < 0) {
    o.run_for_ms = o.stall_ms > 0
                       ? long(o.stall_every_ms * uint64_t(o.stalls) +
                              o.stall_ms * uint64_t(o.stalls) + 700)
                       : 2000;
  }

  sw::Session s(o.name);
  if (!s.ok()) {
    fprintf(stderr, "sw_demo: cannot claim a registry slot\n");
    return 1;
  }

  const uint64_t start = sw::ns_now();
  const uint64_t run_ns = uint64_t(o.run_for_ms) * 1'000'000ull;
  const uint64_t beat_ns = o.beat_us * 1000;
  uint64_t next_stall = o.stall_ms > 0 ? start + o.stall_every_ms * 1'000'000ull : ~0ull;
  uint64_t last_beat = 0;
  uint64_t beats = 0;
  long injected = 0;

  while (true) {
    uint64_t now = sw::ns_now();
    if (now - start >= run_ns) break;
    if (now - last_beat >= beat_ns) {
      if (o.nt)
        s.beat_nt(uint32_t(beats));
      else
        s.beat(uint32_t(beats));
      last_beat = now;
      beats++;
    }
    if (injected < o.stalls && now >= next_stall) {
      stall_here(o.stall_ms * 1'000'000ull);
      injected++;
      next_stall += o.stall_every_ms * 1'000'000ull;
    }
    if (o.sleep_pace)
      usleep(useconds_t(o.beat_us / 2 + 1));
    else
      cpu_relax();
  }

  fprintf(stderr, "sw_demo: done, beats=%llu stalls_injected=%ld slot=%d\n",
          (unsigned long long)beats, injected, s.slot_index());
  return 0;
}
