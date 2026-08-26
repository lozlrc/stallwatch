// stallwatchd: scans the shared registry, flags stalled clients, records
// stall durations, and optionally captures a backtrace from the stalled
// process via the cooperative SIGUSR2 protocol.
#include "detect.hpp"
#include "report.hpp"
#include "shm.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <signal.h>
#include <unistd.h>

namespace {

struct Pending {
  bool active = false;
  sw::StallRec rec;
};

struct Options {
  uint64_t threshold_us = 500;
  uint64_t poll_us = 100;
  bool capture = false;
  const char* report = "stallwatch_report.txt";
  long max_stalls = 0;   // 0 = unlimited
  long run_for_ms = 0;   // 0 = run until killed
  const char* shm = nullptr;
  bool unlink_on_exit = false;
};

void usage(const char* argv0) {
  fprintf(stderr,
          "usage: %s [--threshold-us N] [--poll-us N] [--capture] [--report FILE]\n"
          "          [--max-stalls N] [--run-for-ms N] [--shm NAME] [--unlink-on-exit]\n",
          argv0);
}

bool parse_args(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; i++) {
    auto need = [&](const char* flag) -> const char* {
      if (i + 1 >= argc) {
        fprintf(stderr, "stallwatchd: %s needs a value\n", flag);
        return nullptr;
      }
      return argv[++i];
    };
    if (!strcmp(argv[i], "--threshold-us")) {
      const char* v = need("--threshold-us");
      if (!v) return false;
      o.threshold_us = strtoull(v, nullptr, 10);
    } else if (!strcmp(argv[i], "--poll-us")) {
      const char* v = need("--poll-us");
      if (!v) return false;
      o.poll_us = strtoull(v, nullptr, 10);
    } else if (!strcmp(argv[i], "--capture")) {
      o.capture = true;
    } else if (!strcmp(argv[i], "--report")) {
      const char* v = need("--report");
      if (!v) return false;
      o.report = v;
    } else if (!strcmp(argv[i], "--max-stalls")) {
      const char* v = need("--max-stalls");
      if (!v) return false;
      o.max_stalls = strtol(v, nullptr, 10);
    } else if (!strcmp(argv[i], "--run-for-ms")) {
      const char* v = need("--run-for-ms");
      if (!v) return false;
      o.run_for_ms = strtol(v, nullptr, 10);
    } else if (!strcmp(argv[i], "--shm")) {
      const char* v = need("--shm");
      if (!v) return false;
      o.shm = v;
    } else if (!strcmp(argv[i], "--unlink-on-exit")) {
      o.unlink_on_exit = true;
    } else {
      usage(argv[0]);
      return false;
    }
  }
  return o.threshold_us > 0 && o.poll_us > 0;
}

// Request a backtrace from a stalled client and wait briefly for it.
// The client is mid-stall, so SIGUSR2 interrupts the stalled code and the
// handler writes frames into the slot's spill area.
void do_capture(sw::Registry& reg, uint32_t i, sw::StallRec& rec) {
  sw::Slot& s = reg.slots[i];
  uint32_t expected = sw::ACTIVE;
  if (!s.state.compare_exchange_strong(expected, sw::CAPTURE_REQ, std::memory_order_acq_rel))
    return;
  if (kill(s.pid, SIGUSR2) != 0) {
    expected = sw::CAPTURE_REQ;
    s.state.compare_exchange_strong(expected, sw::ACTIVE, std::memory_order_acq_rel);
    return;
  }
  uint64_t deadline = sw::ns_now() + 50'000'000; // 50 ms
  while (s.state.load(std::memory_order_acquire) != sw::CAPTURE_DONE && sw::ns_now() < deadline)
    usleep(200);
  if (s.state.load(std::memory_order_acquire) == sw::CAPTURE_DONE) {
    uint32_t n = s.capture_len;
    if (n > sw::kFramesCap) n = sw::kFramesCap;
    const uint64_t* f = reg.frames(i);
    rec.frames.assign(f, f + n);
    rec.handler_us = double(s.handler_ns) / 1000.0;
    expected = sw::CAPTURE_DONE;
    s.state.compare_exchange_strong(expected, sw::ACTIVE, std::memory_order_acq_rel);
  } else {
    // Client did not respond (wedged harder than a stall, or exiting).
    expected = sw::CAPTURE_REQ;
    s.state.compare_exchange_strong(expected, sw::ACTIVE, std::memory_order_acq_rel);
  }
}

} // namespace

int main(int argc, char** argv) {
  Options o;
  if (!parse_args(argc, argv, o)) return 2;

  sw::Registry reg;
  if (!sw::registry_open(reg, o.shm, true)) {
    fprintf(stderr, "stallwatchd: cannot open shm registry\n");
    return 1;
  }
  FILE* rf = fopen(o.report, "w");
  if (!rf) {
    fprintf(stderr, "stallwatchd: cannot open report file %s\n", o.report);
    sw::registry_close(reg);
    return 1;
  }
  fprintf(rf, "# stallwatch report v1\n");
  fprintf(rf, "# shm=%s threshold_us=%" PRIu64 " poll_us=%" PRIu64 " capture=%d\n", reg.shm_name,
          o.threshold_us, o.poll_us, int(o.capture));
  fflush(rf);

  static sw::TrackState tracks[sw::kMaxSlots];
  static Pending pending[sw::kMaxSlots];
  const uint64_t threshold_ns = o.threshold_us * 1000;
  const uint64_t start = sw::ns_now();
  uint64_t next_reclaim = start;
  long stalls = 0;
  bool done = false;

  while (!done) {
    uint64_t now = sw::ns_now();
    if (o.run_for_ms > 0 && now - start > uint64_t(o.run_for_ms) * 1'000'000ull) break;
    if (now >= next_reclaim) {
      sw::reclaim_dead_slots(reg);
      next_reclaim = now + 50'000'000; // dead-pid sweep every 50 ms
    }
    for (uint32_t i = 0; i < sw::kMaxSlots; i++) {
      sw::Slot& s = reg.slots[i];
      uint32_t st = s.state.load(std::memory_order_acquire);
      if (st == sw::FREE) {
        tracks[i] = sw::TrackState{};
        pending[i].active = false;
        continue;
      }
      if (st == sw::CAPTURE_DONE) {
        // Stale handshake from an interrupted capture; recover the slot.
        uint32_t e = sw::CAPTURE_DONE;
        s.state.compare_exchange_strong(e, sw::ACTIVE, std::memory_order_acq_rel);
      }
      uint64_t beat = s.beat_ns.load(std::memory_order_relaxed);
      uint64_t seq = s.seq.load(std::memory_order_relaxed);
      sw::StallEvent ev = sw::detect_step(tracks[i], now, beat, seq, threshold_ns);
      if (ev.kind == sw::StallEvent::ONSET) {
        Pending& p = pending[i];
        p.active = true;
        p.rec = sw::StallRec{};
        p.rec.pid = s.pid;
        p.rec.name.assign(s.name, strnlen(s.name, sw::kNameCap));
        p.rec.start_ns = ev.onset_beat;
        p.rec.detect_us = double(ev.detect_latency_ns) / 1000.0;
        p.rec.slide = s.aslr_slide;
        p.rec.exe.assign(reg.exe(i), strnlen(reg.exe(i), sw::kExeCap));
        if (o.capture) do_capture(reg, i, p.rec);
      } else if (ev.kind == sw::StallEvent::RECOVERY && pending[i].active) {
        Pending& p = pending[i];
        p.rec.duration_us = double(ev.duration_ns) / 1000.0;
        fprintf(rf, "%s\n", sw::format_stall_line(p.rec).c_str());
        fflush(rf);
        fprintf(stderr, "stallwatchd: stall pid=%d name=%s duration_us=%.1f detect_us=%.1f\n",
                p.rec.pid, p.rec.name.c_str(), p.rec.duration_us, p.rec.detect_us);
        p.active = false;
        stalls++;
        if (o.max_stalls > 0 && stalls >= o.max_stalls) {
          done = true;
          break;
        }
      }
    }
    if (!done) usleep(useconds_t(o.poll_us));
  }

  fclose(rf);
  if (o.unlink_on_exit) sw::registry_unlink(reg.shm_name);
  sw::registry_close(reg);
  fprintf(stderr, "stallwatchd: exit, %ld stall(s) recorded\n", stalls);
  return 0;
}
