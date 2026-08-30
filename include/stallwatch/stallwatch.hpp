// stallwatch: heartbeat instrumentation for event-loop processes.
// Single public client header. Registry layout, claim/release, and the
// Session client live here so instrumented programs need only this file.
// Monitor-side helpers are in src/shm.hpp (they link src/shm.cpp).
#pragma once

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <pthread.h>
#else
#include <sys/syscall.h>
#endif
#if defined(__x86_64__)
#include <immintrin.h> // _mm_stream_si64; x86 path compiles but is untested on this arm64 machine
#endif

namespace sw {

// ---------------------------------------------------------------------------
// Registry layout
// ---------------------------------------------------------------------------

constexpr uint32_t kMagic     = 0x53575231u; // "SWR1"
constexpr uint32_t kMagicInit = 0x53575200u; // init-in-progress sentinel
constexpr uint32_t kVersion   = 1;
constexpr uint32_t kMaxSlots  = 64;
constexpr uint32_t kFramesCap = 128; // spill frame addresses per slot
constexpr uint32_t kExeCap    = 256; // spill exe path bytes per slot
constexpr uint32_t kNameCap   = 24;

enum SlotState : uint32_t {
  FREE         = 0,
  ACTIVE       = 1,
  CAPTURE_REQ  = 2, // monitor wants a backtrace, signal sent
  CAPTURE_DONE = 3, // client handler filled the spill area
};

struct Header {
  std::atomic<uint32_t> magic; // set to kMagic (release) after fields below
  uint32_t version;
  uint32_t max_slots;
  uint32_t slot_stride;
  uint32_t spill_stride;
  uint32_t frames_cap;
  uint32_t exe_cap;
  uint32_t reserved;
  uint8_t pad[32];
};
static_assert(sizeof(Header) == 64, "header is one cacheline");

// One slot is exactly two 64-byte cachelines. Line A is written by the client
// on every beat and read by the monitor every poll. Line B is written once at
// registration plus the capture handshake, so beats never touch it.
struct alignas(64) Slot {
  // Line A (hot).
  std::atomic<uint64_t> beat_ns;
  std::atomic<uint64_t> seq;
  uint32_t tag;
  uint32_t pad_a;
  uint8_t pad_a2[40];
  // Line B (cold).
  int32_t pid;
  uint32_t pad_b;
  uint64_t tid;
  char name[kNameCap];
  uint64_t aslr_slide; // main image load address, see README (atos -l / addr2line base)
  std::atomic<uint32_t> state;
  uint32_t capture_len;
  uint32_t exe_len;
  uint32_t handler_ns; // duration of the last capture handler run
};
static_assert(sizeof(Slot) == 128, "slot is two cachelines");

constexpr size_t kSpillStride = size_t(kFramesCap) * 8 + kExeCap; // frames then exe path
constexpr size_t kSlotsOff    = sizeof(Header);
constexpr size_t kSpillOff    = kSlotsOff + size_t(kMaxSlots) * sizeof(Slot);
constexpr size_t kSegSize     = kSpillOff + size_t(kMaxSlots) * kSpillStride;

struct Registry {
  void* base = nullptr;
  Header* hdr = nullptr;
  Slot* slots = nullptr;
  uint8_t* spill = nullptr;
  int fd = -1;
  char shm_name[64] = {};

  bool valid() const noexcept { return base != nullptr; }
  uint64_t* frames(uint32_t i) noexcept {
    return reinterpret_cast<uint64_t*>(spill + i * kSpillStride);
  }
  char* exe(uint32_t i) noexcept {
    return reinterpret_cast<char*>(spill + i * kSpillStride + size_t(kFramesCap) * 8);
  }
};

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

inline uint64_t ns_now() noexcept {
#if defined(__APPLE__)
  return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
#endif
}

// ---------------------------------------------------------------------------
// Registry open/close
// ---------------------------------------------------------------------------

inline void default_shm_name(char* out, size_t cap) noexcept {
  const char* env = getenv("STALLWATCH_SHM");
  if (env && env[0]) {
    snprintf(out, cap, "%s", env);
    return;
  }
  snprintf(out, cap, "/stallwatch.%u", unsigned(getuid()));
}

inline void registry_close(Registry& r) noexcept {
  if (r.base) munmap(r.base, kSegSize);
  if (r.fd >= 0) close(r.fd);
  r = Registry{};
}

inline void registry_unlink(const char* name) noexcept { shm_unlink(name); }

// Open (and create if asked) the shared registry. First opener sizes the
// segment and initializes the header behind an init sentinel so late openers
// never observe half-written header fields.
inline bool registry_open(Registry& r, const char* name, bool create) noexcept {
  char nb[64];
  if (!name) {
    default_shm_name(nb, sizeof nb);
    name = nb;
  }
  snprintf(r.shm_name, sizeof r.shm_name, "%s", name);
  int fd = shm_open(name, O_RDWR | (create ? O_CREAT : 0), 0600);
  if (fd < 0) return false;
  struct stat st = {};
  if (fstat(fd, &st) != 0) { close(fd); return false; }
  if (size_t(st.st_size) < kSegSize) {
    // macOS allows ftruncate on a shm object only once; on a lost race,
    // re-stat and accept the winner's size.
    if (ftruncate(fd, off_t(kSegSize)) != 0) {
      if (fstat(fd, &st) != 0 || size_t(st.st_size) < kSegSize) { close(fd); return false; }
    }
  }
  void* p = mmap(nullptr, kSegSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) { close(fd); return false; }
  r.base = p;
  r.fd = fd;
  r.hdr = reinterpret_cast<Header*>(p);
  r.slots = reinterpret_cast<Slot*>(static_cast<uint8_t*>(p) + kSlotsOff);
  r.spill = static_cast<uint8_t*>(p) + kSpillOff;

  if (r.hdr->magic.load(std::memory_order_acquire) != kMagic) {
    uint32_t expected = 0;
    if (r.hdr->magic.compare_exchange_strong(expected, kMagicInit, std::memory_order_acq_rel)) {
      r.hdr->version = kVersion;
      r.hdr->max_slots = kMaxSlots;
      r.hdr->slot_stride = uint32_t(sizeof(Slot));
      r.hdr->spill_stride = uint32_t(kSpillStride);
      r.hdr->frames_cap = kFramesCap;
      r.hdr->exe_cap = kExeCap;
      r.hdr->reserved = 0;
      r.hdr->magic.store(kMagic, std::memory_order_release);
    } else {
      // Someone else is initializing; wait briefly.
      for (int i = 0; i < 10000 && r.hdr->magic.load(std::memory_order_acquire) != kMagic; i++)
        usleep(100);
      if (r.hdr->magic.load(std::memory_order_acquire) != kMagic) { registry_close(r); return false; }
    }
  }
  if (r.hdr->version != kVersion || r.hdr->max_slots != kMaxSlots ||
      r.hdr->slot_stride != sizeof(Slot) || r.hdr->spill_stride != kSpillStride) {
    registry_close(r);
    return false;
  }
  return true;
}

#if !defined(__APPLE__)
// Base address of the main executable mapping, for addr2line adjustment.
// Compiles on Linux only; untested on this machine.
inline uint64_t linux_main_base() noexcept {
  char exe[256];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (n <= 0) return 0;
  exe[n] = 0;
  FILE* f = fopen("/proc/self/maps", "r");
  if (!f) return 0;
  char line[512];
  uint64_t base = 0;
  while (fgets(line, sizeof line, f)) {
    unsigned long long lo = 0, hi = 0;
    char perms[8] = {};
    char path[384] = {};
    if (sscanf(line, "%llx-%llx %7s %*s %*s %*s %383s", &lo, &hi, perms, path) >= 3) {
      if (path[0] && strcmp(path, exe) == 0) { base = lo; break; } // maps are sorted, first hit is lowest
    }
  }
  fclose(f);
  return base;
}
#endif

// ---------------------------------------------------------------------------
// Slot claim/release
// ---------------------------------------------------------------------------

inline int claim_slot(Registry& r, const char* name, uint32_t tag = 0) noexcept {
  for (uint32_t i = 0; i < kMaxSlots; i++) {
    Slot& s = r.slots[i];
    uint32_t expected = FREE;
    if (!s.state.compare_exchange_strong(expected, ACTIVE, std::memory_order_acq_rel)) continue;
    // Line B fill happens after the CAS. The monitor tolerates that: it never
    // judges a slot on its first observation and skips beat_ns == 0.
    s.pid = int32_t(getpid());
#if defined(__APPLE__)
    uint64_t tid = 0;
    pthread_threadid_np(nullptr, &tid);
    s.tid = tid;
#else
    s.tid = uint64_t(syscall(SYS_gettid));
#endif
    memset(s.name, 0, kNameCap);
    if (name) {
      strncpy(s.name, name, kNameCap - 1);
      for (char* c = s.name; *c; c++)
        if (*c == ' ' || *c == '\t') *c = '_'; // keep report lines space-delimited
    }
    s.tag = tag;
    s.capture_len = 0;
    s.handler_ns = 0;
    char* exe = r.exe(i);
    memset(exe, 0, kExeCap);
#if defined(__APPLE__)
    char tmp[1024];
    uint32_t tcap = sizeof tmp;
    if (_NSGetExecutablePath(tmp, &tcap) == 0) strncpy(exe, tmp, kExeCap - 1);
    s.aslr_slide = uint64_t(reinterpret_cast<uintptr_t>(_dyld_get_image_header(0)));
#else
    ssize_t n = readlink("/proc/self/exe", exe, kExeCap - 1);
    if (n < 0) exe[0] = 0;
    s.aslr_slide = linux_main_base();
#endif
    s.exe_len = uint32_t(strnlen(exe, kExeCap));
    s.seq.store(1, std::memory_order_relaxed);
    s.beat_ns.store(ns_now(), std::memory_order_relaxed);
    return int(i);
  }
  return -1; // registry full
}

inline void release_slot(Registry& r, int idx) noexcept {
  if (idx < 0 || uint32_t(idx) >= kMaxSlots) return;
  r.slots[idx].state.store(FREE, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Beat stores
// ---------------------------------------------------------------------------

inline void beat_store_plain(std::atomic<uint64_t>* a, uint64_t v) noexcept {
  a->store(v, std::memory_order_relaxed);
}

// Non-temporal variant. Casting the atomic to a plain u64 is formally outside
// the standard but layout-compatible on both targets; asserted below.
// x86: movnti via _mm_stream_si64 (clang and gcc). clang elsewhere: the
// nontemporal builtin (lowers to stnp on aarch64). gcc/aarch64 has no portable
// single-word NT store builtin, so it falls back to a plain store; that is
// honest here since NT showed no measured benefit on this arm64 core.
inline void beat_store_nt(std::atomic<uint64_t>* a, uint64_t v) noexcept {
  static_assert(sizeof(std::atomic<uint64_t>) == sizeof(uint64_t));
#if defined(__x86_64__)
  _mm_stream_si64(reinterpret_cast<long long*>(a), (long long)(v)); // untested here (arm64 machine)
#elif defined(__clang__)
  __builtin_nontemporal_store(v, reinterpret_cast<uint64_t*>(a));
#else
  a->store(v, std::memory_order_relaxed);
#endif
}

// ---------------------------------------------------------------------------
// Capture signal handler
// ---------------------------------------------------------------------------

inline std::atomic<Slot*> g_capture_slot{nullptr};
inline std::atomic<uint64_t*> g_capture_frames{nullptr};

// SIGUSR2 handler. backtrace() is not formally async-signal-safe (glibc may
// dlopen a helper on first use, and the unwinder takes no locks but is not on
// the POSIX safe list). The Session constructor warms it up once outside
// signal context, and this is a diagnostic tool, so the risk is accepted.
inline void capture_signal_handler(int) noexcept {
  int saved_errno = errno;
  Slot* s = g_capture_slot.load(std::memory_order_acquire);
  uint64_t* out = g_capture_frames.load(std::memory_order_acquire);
  if (s && out && s->state.load(std::memory_order_acquire) == CAPTURE_REQ) {
    uint64_t t0 = ns_now();
    void* buf[kFramesCap];
    int n = ::backtrace(buf, int(kFramesCap));
    if (n < 0) n = 0;
    for (int i = 0; i < n; i++) out[i] = uint64_t(reinterpret_cast<uintptr_t>(buf[i]));
    uint64_t dt = ns_now() - t0;
    s->handler_ns = dt > 0xffffffffull ? 0xffffffffu : uint32_t(dt);
    s->capture_len = uint32_t(n);
    s->state.store(CAPTURE_DONE, std::memory_order_release);
  }
  errno = saved_errno;
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

// One Session per process (the capture handler uses process globals).
// Check ok() once after construction; beat() assumes a claimed slot.
class Session {
 public:
  explicit Session(const char* name, uint32_t tag = 0) noexcept {
    if (!registry_open(reg_, nullptr, true)) return;
    idx_ = claim_slot(reg_, name, tag);
    if (idx_ < 0) { registry_close(reg_); return; }
    slot_ = &reg_.slots[idx_];
    seq_ = 1;
    g_capture_slot.store(slot_, std::memory_order_release);
    g_capture_frames.store(reg_.frames(uint32_t(idx_)), std::memory_order_release);
    struct sigaction sa = {};
    sa.sa_handler = capture_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR2, &sa, &old_sa_);
    void* warm[4];
    (void)::backtrace(warm, 4); // warm up the unwinder outside signal context
  }

  ~Session() {
    if (!slot_) return;
    g_capture_slot.store(nullptr, std::memory_order_release);
    g_capture_frames.store(nullptr, std::memory_order_release);
    sigaction(SIGUSR2, &old_sa_, nullptr);
    release_slot(reg_, idx_);
    registry_close(reg_);
    slot_ = nullptr;
  }

  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  bool ok() const noexcept { return slot_ != nullptr; }
  int slot_index() const noexcept { return idx_; }
  Registry& registry() noexcept { return reg_; }

  // One beat: one clock read, one relaxed 8-byte store, one relaxed seq store.
  // SW_NT=1 switches the timestamp store to the non-temporal variant.
  void beat(uint32_t tag = 0) noexcept {
#if defined(SW_NT) && SW_NT
    beat_nt(tag);
#else
    beat_plain(tag);
#endif
  }

  void beat_plain(uint32_t tag = 0) noexcept {
    slot_->tag = tag;
    beat_store_plain(&slot_->beat_ns, ns_now());
    slot_->seq.store(++seq_, std::memory_order_relaxed);
  }

  void beat_nt(uint32_t tag = 0) noexcept {
    slot_->tag = tag;
    beat_store_nt(&slot_->beat_ns, ns_now());
    slot_->seq.store(++seq_, std::memory_order_relaxed);
  }

 private:
  Registry reg_ = {};
  Slot* slot_ = nullptr;
  int idx_ = -1;
  uint64_t seq_ = 0;
  struct sigaction old_sa_ = {};
};

} // namespace sw
