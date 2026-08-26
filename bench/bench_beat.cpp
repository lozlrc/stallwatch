// bench_beat: cost of Session::beat() for the plain and non-temporal store
// variants, plus a cache-interference probe (4 MB scan between beats).
#include "stallwatch/stallwatch.hpp"

#include <cstdio>
#include <cstdlib>
#include <sys/utsname.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

static constexpr uint64_t kIters = 200'000'000ull;
static constexpr size_t kScanWords = 4u << 20 >> 3; // 4 MB of u64
static constexpr int kScanReps = 600;

__attribute__((noinline)) static uint64_t loop_baseline() {
  uint64_t x = 0;
  for (uint64_t i = 0; i < kIters; i++) {
    asm volatile("" : "+r"(x));
    x++;
  }
  return x;
}

__attribute__((noinline)) static uint64_t loop_clock() {
  uint64_t x = 0;
  for (uint64_t i = 0; i < kIters; i++) x ^= sw::ns_now();
  return x;
}

__attribute__((noinline)) static void loop_plain(sw::Session& s) {
  for (uint64_t i = 0; i < kIters; i++) s.beat_plain(uint32_t(i));
}

__attribute__((noinline)) static void loop_nt(sw::Session& s) {
  for (uint64_t i = 0; i < kIters; i++) s.beat_nt(uint32_t(i));
}

__attribute__((noinline)) static uint64_t scan(const uint64_t* a) {
  uint64_t sum = 0;
  for (size_t i = 0; i < kScanWords; i++) sum += a[i];
  return sum;
}

enum class BeatMode { NONE, PLAIN, NT };

__attribute__((noinline)) static double scan_probe(sw::Session& s, const uint64_t* a,
                                                   BeatMode m, uint64_t* sink) {
  uint64_t t0 = sw::ns_now();
  uint64_t acc = 0;
  for (int r = 0; r < kScanReps; r++) {
    if (m == BeatMode::PLAIN)
      s.beat_plain(uint32_t(r));
    else if (m == BeatMode::NT)
      s.beat_nt(uint32_t(r));
    acc += scan(a);
    asm volatile("" : "+r"(acc));
  }
  uint64_t t1 = sw::ns_now();
  *sink ^= acc;
  double bytes = double(kScanReps) * double(kScanWords) * 8.0;
  return bytes / double(t1 - t0); // GB/s (bytes per ns)
}

static void print_machine() {
  struct utsname u = {};
  uname(&u);
#if defined(__APPLE__)
  char brand[128] = "unknown";
  size_t blen = sizeof brand;
  sysctlbyname("machdep.cpu.brand_string", brand, &blen, nullptr, 0);
  printf("machine: %s, %s, %s %s\n", brand, u.machine, u.sysname, u.release);
#else
  printf("machine: %s, %s %s\n", u.machine, u.sysname, u.release);
#endif
  printf("compiler: %s\n", __VERSION__);
}

int main() {
  char nm[64];
  snprintf(nm, sizeof nm, "/swb.%d", int(getpid()));
  sw::registry_unlink(nm);
  setenv("STALLWATCH_SHM", nm, 1);

  sw::Session s("bench_beat");
  if (!s.ok()) {
    fprintf(stderr, "bench_beat: cannot claim a registry slot\n");
    return 1;
  }

  printf("== bench_beat\n");
  print_machine();
  printf("iters: %llu\n", (unsigned long long)kIters);

  uint64_t sink = 0;
  uint64_t t0, t1;

  t0 = sw::ns_now();
  sink ^= loop_baseline();
  t1 = sw::ns_now();
  printf("empty_loop      %6.2f ns/iter\n", double(t1 - t0) / double(kIters));

  t0 = sw::ns_now();
  sink ^= loop_clock();
  t1 = sw::ns_now();
  printf("clock_read      %6.2f ns/iter  (ns_now only)\n", double(t1 - t0) / double(kIters));

  t0 = sw::ns_now();
  loop_plain(s);
  t1 = sw::ns_now();
  printf("beat_plain      %6.2f ns/iter\n", double(t1 - t0) / double(kIters));

  t0 = sw::ns_now();
  loop_nt(s);
  t1 = sw::ns_now();
  printf("beat_nt         %6.2f ns/iter\n", double(t1 - t0) / double(kIters));

  uint64_t* arr = static_cast<uint64_t*>(malloc(kScanWords * 8));
  for (size_t i = 0; i < kScanWords; i++) arr[i] = i;
  sink ^= scan(arr); // warm

  double g_none = scan_probe(s, arr, BeatMode::NONE, &sink);
  double g_plain = scan_probe(s, arr, BeatMode::PLAIN, &sink);
  double g_nt = scan_probe(s, arr, BeatMode::NT, &sink);
  printf("scan_4mb_alone  %6.1f GB/s\n", g_none);
  printf("scan_4mb_plain  %6.1f GB/s  (one beat_plain per 4 MB scan)\n", g_plain);
  printf("scan_4mb_nt     %6.1f GB/s  (one beat_nt per 4 MB scan)\n", g_nt);

  free(arr);
  if (sink == 0x5a5a5a5a) printf("\n"); // keep sink observable
  sw::registry_unlink(nm);
  return 0;
}
