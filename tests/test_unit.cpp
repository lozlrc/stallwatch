// Unit tests: registry claim/release/reclaim, detection state machine with
// fake timestamps, report line round trip, Session end to end in-process.
#include "detect.hpp"
#include "report.hpp"
#include "shm.hpp"

#include <cmath>
#include <cstdio>
#include <sys/wait.h>

static int g_checks = 0;
static int g_fails = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    g_checks++;                                                        \
    if (!(cond)) {                                                     \
      g_fails++;                                                       \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                  \
  } while (0)

static void make_name(char* out, size_t cap, const char* prefix) {
  snprintf(out, cap, "/%s.%d", prefix, int(getpid()));
}

static void test_layout() {
  CHECK(sizeof(sw::Header) == 64);
  CHECK(sizeof(sw::Slot) == 128);
  CHECK(sw::kSegSize == 64 + 64 * 128 + 64 * (128 * 8 + 256));
}

static void test_registry_claim_release() {
  char nm[64];
  make_name(nm, sizeof nm, "swua");
  sw::registry_unlink(nm);
  sw::Registry r;
  CHECK(sw::registry_open(r, nm, true));
  CHECK(r.hdr->magic.load() == sw::kMagic);
  CHECK(r.hdr->max_slots == 64);

  int a = sw::claim_slot(r, "unit a");
  CHECK(a == 0);
  CHECK(r.slots[a].state.load() == sw::ACTIVE);
  CHECK(r.slots[a].pid == int32_t(getpid()));
  CHECK(strcmp(r.slots[a].name, "unit_a") == 0); // spaces sanitized
  CHECK(r.slots[a].beat_ns.load() != 0);
  CHECK(r.slots[a].seq.load() == 1);
  CHECK(r.slots[a].exe_len > 0);
  CHECK(r.exe(uint32_t(a))[0] == '/');
  CHECK(r.slots[a].aslr_slide != 0);

  int b = sw::claim_slot(r, "unit_b");
  CHECK(b == 1);
  sw::release_slot(r, a);
  CHECK(r.slots[a].state.load() == sw::FREE);
  int c = sw::claim_slot(r, "unit_c"); // first free slot is reused
  CHECK(c == 0);
  sw::release_slot(r, b);
  sw::release_slot(r, c);
  sw::registry_close(r);
  sw::registry_unlink(nm);
}

static void test_registry_full() {
  char nm[64];
  make_name(nm, sizeof nm, "swuf");
  sw::registry_unlink(nm);
  sw::Registry r;
  CHECK(sw::registry_open(r, nm, true));
  for (int i = 0; i < 64; i++) CHECK(sw::claim_slot(r, "filler") == i);
  CHECK(sw::claim_slot(r, "one_too_many") == -1); // clean error, no crash
  for (int i = 0; i < 64; i++) sw::release_slot(r, i);
  sw::registry_close(r);
  sw::registry_unlink(nm);
}

static void test_reclaim_dead() {
  char nm[64];
  make_name(nm, sizeof nm, "swud");
  sw::registry_unlink(nm);
  sw::Registry r;
  CHECK(sw::registry_open(r, nm, true));
  int a = sw::claim_slot(r, "live");
  int b = sw::claim_slot(r, "dead");
  CHECK(a == 0 && b == 1);

  // A forked child that has exited and been reaped is a guaranteed-dead pid.
  pid_t child = fork();
  if (child == 0) _exit(0);
  CHECK(child > 0);
  int st = 0;
  CHECK(waitpid(child, &st, 0) == child);
  r.slots[b].pid = int32_t(child);

  CHECK(sw::pid_alive(int32_t(getpid())));
  CHECK(!sw::pid_alive(int32_t(child)));
  int n = sw::reclaim_dead_slots(r);
  CHECK(n == 1);
  CHECK(r.slots[b].state.load() == sw::FREE);
  CHECK(r.slots[a].state.load() == sw::ACTIVE); // live slot untouched
  sw::release_slot(r, a);
  sw::registry_close(r);
  sw::registry_unlink(nm);
}

static void test_detect_state_machine() {
  const uint64_t T = 500'000; // 500 us threshold
  sw::TrackState st;
  sw::StallEvent ev;

  // First observation is never judged, even if the beat looks ancient.
  ev = sw::detect_step(st, 10'000'000, 1'000, 5, T);
  CHECK(ev.kind == sw::StallEvent::NONE);

  // Healthy beats: seq advances, no stall.
  ev = sw::detect_step(st, 10'100'000, 10'050'000, 6, T);
  CHECK(ev.kind == sw::StallEvent::NONE);

  // Beat stale but seq changed since last poll: not a stall.
  ev = sw::detect_step(st, 11'000'000, 10'050'000, 7, T);
  CHECK(ev.kind == sw::StallEvent::NONE);

  // Stale beat, seq unchanged: onset, with exact detect latency.
  // now=11.7ms, beat=10.05ms, threshold 0.5ms: latency = 11.7 - 10.55 = 1.15ms.
  ev = sw::detect_step(st, 11'700'000, 10'050'000, 7, T);
  CHECK(ev.kind == sw::StallEvent::ONSET);
  CHECK(ev.detect_latency_ns == 1'150'000);
  CHECK(ev.onset_beat == 10'050'000);

  // Still stalled: no repeat onset.
  ev = sw::detect_step(st, 12'000'000, 10'050'000, 7, T);
  CHECK(ev.kind == sw::StallEvent::NONE);

  // Recovery: duration is beat-to-beat.
  ev = sw::detect_step(st, 15'100'000, 15'050'000, 8, T);
  CHECK(ev.kind == sw::StallEvent::RECOVERY);
  CHECK(ev.duration_ns == 15'050'000 - 10'050'000);

  // beat_ns == 0 (mid-registration) is never a stall.
  sw::TrackState st2;
  (void)sw::detect_step(st2, 1'000'000, 0, 0, T);
  ev = sw::detect_step(st2, 2'000'000, 0, 0, T);
  CHECK(ev.kind == sw::StallEvent::NONE);
}

static void test_report_round_trip() {
  sw::StallRec r;
  r.pid = 4242;
  r.name = "demo_loop";
  r.start_ns = 123456789012345ull;
  r.duration_us = 5012.3;
  r.detect_us = 142.7;
  r.handler_us = 9.4;
  r.slide = 0x104a3c000ull;
  r.exe = "/tmp/some dir/sw_demo"; // space in path must survive
  r.frames = {0x104a3d1f0ull, 0x104a3d2a4ull, 0x19b3f4de0ull};

  std::string line = sw::format_stall_line(r);
  sw::StallRec p;
  CHECK(sw::parse_stall_line(line, p));
  CHECK(p.pid == r.pid);
  CHECK(p.name == r.name);
  CHECK(p.start_ns == r.start_ns);
  CHECK(std::fabs(p.duration_us - r.duration_us) < 0.05);
  CHECK(std::fabs(p.detect_us - r.detect_us) < 0.05);
  CHECK(std::fabs(p.handler_us - r.handler_us) < 0.05);
  CHECK(p.slide == r.slide);
  CHECK(p.exe == r.exe);
  CHECK(p.frames == r.frames);

  // No frames encodes as "-" and round-trips to empty.
  r.frames.clear();
  CHECK(sw::parse_stall_line(sw::format_stall_line(r), p));
  CHECK(p.frames.empty());

  // Garbage is rejected.
  CHECK(!sw::parse_stall_line("stall pid=1 nothing else", p));
  CHECK(!sw::parse_stall_line("# comment line", p));
  CHECK(!sw::parse_stall_line("", p));
}

static void test_session_beats() {
  char nm[64];
  make_name(nm, sizeof nm, "swus");
  sw::registry_unlink(nm);
  setenv("STALLWATCH_SHM", nm, 1);
  {
    sw::Session s("unit_session");
    CHECK(s.ok());
    CHECK(s.slot_index() == 0);
    s.beat(7);
    s.beat_plain(8);
    s.beat_nt(9);
    sw::Registry view; // independent mapping of the same object
    CHECK(sw::registry_open(view, nm, false));
    sw::Slot& sl = view.slots[0];
    CHECK(sl.state.load() == sw::ACTIVE);
    CHECK(sl.seq.load() == 4); // 1 at claim + 3 beats
    CHECK(sl.beat_ns.load() != 0);
    CHECK(sl.tag == 9);
    CHECK(strcmp(sl.name, "unit_session") == 0);
    sw::registry_close(view);
  }
  // Destructor released the slot.
  sw::Registry view;
  CHECK(sw::registry_open(view, nm, false));
  CHECK(view.slots[0].state.load() == sw::FREE);
  sw::registry_close(view);
  sw::registry_unlink(nm);
  unsetenv("STALLWATCH_SHM");
}

int main() {
  test_layout();
  test_registry_claim_release();
  test_registry_full();
  test_reclaim_dead();
  test_detect_state_machine();
  test_report_round_trip();
  test_session_beats();
  if (g_fails) {
    fprintf(stderr, "unit: %d/%d checks FAILED\n", g_fails, g_checks);
    return 1;
  }
  printf("unit: %d checks passed\n", g_checks);
  return 0;
}
