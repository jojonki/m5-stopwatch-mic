#pragma once
// Push-to-talk gating shared by the wireless firmwares.
//
// Audio is forwarded only while the button is held, plus
//   - a pre-roll: the last PREROLL_CHUNKS chunks before the press, so the
//     first syllable is not lost, and
//   - a tail: RELEASE_TAIL_US after release, with the trigger still reported
//     as held, so the host app keeps listening until the last audio is out.
// The trigger-down edge is reported immediately from the button handler; the
// trigger-up edge is reported after the tail.

#include <stdint.h>

#include "mic.h"

namespace swmic {

class TriggerGate {
 public:
  static constexpr int     PREROLL_CHUNKS = 5;            // 100 ms
  static constexpr int64_t RELEASE_TAIL_US = 300 * 1000;  // 300 ms

  // Transport hooks. Called from the mic task, except trigger(true) on a
  // press, which comes from on_button().
  struct Sink {
    virtual void begin_utterance() {}  // before the first audio of a press
    virtual void audio(const int16_t *samples, bool trigger) = 0;
    virtual void trigger(bool down) = 0;
    virtual ~Sink() = default;
  };

  explicit TriggerGate(Sink &sink) : sink_(sink) {}

  // From the button polling loop.
  void on_button(bool down);
  // From the mic chunk callback.
  void on_chunk(const int16_t *samples);

 private:
  Sink &sink_;
  volatile bool key_down_ = false;
  volatile bool press_pending_ = false;  // latches taps shorter than a chunk
  bool active_ = false;
  int64_t release_at_ = 0;
  int16_t preroll_[PREROLL_CHUNKS][CHUNK_SAMPLES];
  int preroll_head_ = 0;
  int preroll_count_ = 0;
};

}  // namespace swmic
