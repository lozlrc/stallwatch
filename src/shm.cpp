#include "shm.hpp"

#include <cerrno>
#include <signal.h>

namespace sw {

bool pid_alive(int32_t pid) noexcept {
  if (pid <= 0) return false; // half-filled slot, treat as dead only when pid is set
  if (kill(pid, 0) == 0) return true;
  return errno != ESRCH;
}

int reclaim_dead_slots(Registry& r) noexcept {
  int n = 0;
  for (uint32_t i = 0; i < kMaxSlots; i++) {
    Slot& s = r.slots[i];
    uint32_t st = s.state.load(std::memory_order_acquire);
    if (st == FREE) continue;
    if (s.pid > 0 && !pid_alive(s.pid)) {
      s.state.store(FREE, std::memory_order_release);
      n++;
    }
  }
  return n;
}

} // namespace sw
