// Non-cooperative backtrace capture. The monitor freezes the target with
// ptrace and walks its stack from this process via libunwind-ptrace, so the
// target needs no signal handler and can be arbitrarily wedged. Linux only;
// elsewhere remote_unwind() returns ENOSYS and supported() is false.
#pragma once

#include <cstdint>
#include <vector>

namespace sw {

struct RemoteCapture {
  std::vector<uint64_t> frames;
  uint64_t stop_ns = 0; // how long the target was frozen (interrupt to detach)
  int err = 0;          // 0 on success, else errno-style cause
};

bool remote_unwind_supported() noexcept;
RemoteCapture remote_unwind(int pid, uint32_t max_frames);

} // namespace sw
