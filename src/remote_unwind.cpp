// Remote unwind implementation. See remote_unwind.hpp for the contract.
//
// Sequence: PTRACE_SEIZE (does not stop the target), PTRACE_INTERRUPT
// (group-stop without injecting a signal), waitpid for the stop, walk the
// stack with libunwind's ptrace accessors, PTRACE_DETACH. If the stop we
// observe is an ordinary signal-delivery stop that raced with the interrupt,
// the signal is re-delivered on detach so nothing is swallowed.
//
// The target is frozen only between the interrupt and the detach; stop_ns
// reports that window. Attaches the process (its main thread); unwinding a
// specific secondary thread would seize the tid instead, which is noted as a
// limitation in the README.
#include "remote_unwind.hpp"

#include "stallwatch/stallwatch.hpp" // ns_now

#if defined(__linux__) && defined(SW_REMOTE)

#define UNW_REMOTE_ONLY
#include <libunwind-ptrace.h>

#include <cerrno>
#include <sys/ptrace.h>
#include <sys/wait.h>

namespace sw {

bool remote_unwind_supported() noexcept { return true; }

RemoteCapture remote_unwind(int pid, uint32_t max_frames) {
  RemoteCapture rc;
  const uint64_t t0 = ns_now();

  if (ptrace(PTRACE_SEIZE, pid, nullptr, nullptr) != 0) {
    rc.err = errno;
    return rc;
  }
  if (ptrace(PTRACE_INTERRUPT, pid, nullptr, nullptr) != 0) {
    rc.err = errno;
    ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
    return rc;
  }
  int status = 0;
  if (waitpid(pid, &status, __WALL) < 0 || !WIFSTOPPED(status)) {
    rc.err = errno ? errno : ECHILD;
    ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
    return rc;
  }
  // PTRACE_EVENT_STOP marks the interrupt stop; anything else stopped on a
  // real signal, which must be re-delivered when we detach.
  const bool event_stop = (status >> 16) == PTRACE_EVENT_STOP;
  const int deliver = (!event_stop && WSTOPSIG(status) != SIGTRAP) ? WSTOPSIG(status) : 0;

  unw_addr_space_t as = unw_create_addr_space(&_UPT_accessors, 0);
  if (as != nullptr) {
    void* ctx = _UPT_create(pid);
    if (ctx != nullptr) {
      unw_cursor_t cursor;
      if (unw_init_remote(&cursor, as, ctx) == 0) {
        do {
          unw_word_t ip = 0;
          if (unw_get_reg(&cursor, UNW_REG_IP, &ip) != 0) break;
          rc.frames.push_back(uint64_t(ip));
        } while (rc.frames.size() < max_frames && unw_step(&cursor) > 0);
      } else {
        rc.err = EIO;
      }
      _UPT_destroy(ctx);
    } else {
      rc.err = ENOMEM;
    }
    unw_destroy_addr_space(as);
  } else {
    rc.err = ENOMEM;
  }

  ptrace(PTRACE_DETACH, pid, nullptr, reinterpret_cast<void*>(intptr_t(deliver)));
  rc.stop_ns = ns_now() - t0;
  if (!rc.frames.empty()) rc.err = 0;
  return rc;
}

} // namespace sw

#else // stub for platforms without the ptrace path

#include <cerrno>

namespace sw {

bool remote_unwind_supported() noexcept { return false; }

RemoteCapture remote_unwind(int, uint32_t) {
  RemoteCapture rc;
  rc.err = ENOSYS;
  return rc;
}

} // namespace sw

#endif
