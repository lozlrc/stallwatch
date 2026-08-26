// Monitor-side registry helpers. Layout and client operations live in the
// public header so instrumented programs stay single-header; this TU pair
// carries what only the monitor and tests need.
#pragma once

#include "stallwatch/stallwatch.hpp"

namespace sw {

// kill(pid, 0) liveness. EPERM counts as alive.
bool pid_alive(int32_t pid) noexcept;

// Free every non-FREE slot whose pid is gone. Returns slots reclaimed.
int reclaim_dead_slots(Registry& r) noexcept;

} // namespace sw
