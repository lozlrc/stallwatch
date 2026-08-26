// Stall detection state machine, factored as a pure function so the unit
// tests can drive it with fake timestamps.
#pragma once

#include <cstdint>

namespace sw {

struct TrackState {
  bool seen = false;     // one observation recorded, judgments start on the second
  bool in_stall = false;
  uint64_t last_seq = 0;
  uint64_t last_beat = 0;
  uint64_t onset_beat = 0; // last beat before the stall
  uint64_t t_detect = 0;
};

struct StallEvent {
  enum Kind { NONE, ONSET, RECOVERY } kind = NONE;
  uint64_t detect_latency_ns = 0; // ONSET: now - (last_beat + threshold)
  uint64_t duration_ns = 0;       // RECOVERY: first beat after - last beat before
  uint64_t onset_beat = 0;
};

// One poll observation for one slot. Rules:
// - Never judge the first observation (slot may be mid-registration).
// - Stall onset requires the beat to be older than the threshold AND seq
//   unchanged since the previous poll. A fresh seq means the client wrote a
//   beat between our reads, so a stale-looking timestamp is not a stall.
// - Recovery is seq advancing; duration is measured beat-to-beat on the
//   client's own clock, so monitor scheduling never inflates it.
inline StallEvent detect_step(TrackState& st, uint64_t now, uint64_t beat, uint64_t seq,
                              uint64_t threshold_ns) {
  StallEvent ev;
  if (!st.seen) {
    st.seen = true;
    st.last_seq = seq;
    st.last_beat = beat;
    return ev;
  }
  if (!st.in_stall) {
    if (seq == st.last_seq && beat != 0 && now > beat && now - beat > threshold_ns) {
      st.in_stall = true;
      st.onset_beat = beat;
      st.t_detect = now;
      ev.kind = StallEvent::ONSET;
      ev.detect_latency_ns = now - (beat + threshold_ns);
      ev.onset_beat = beat;
    }
  } else if (seq != st.last_seq) {
    st.in_stall = false;
    ev.kind = StallEvent::RECOVERY;
    ev.duration_ns = beat > st.onset_beat ? beat - st.onset_beat : 0;
    ev.onset_beat = st.onset_beat;
  }
  st.last_seq = seq;
  st.last_beat = beat;
  return ev;
}

} // namespace sw
