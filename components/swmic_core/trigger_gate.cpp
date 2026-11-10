#include "trigger_gate.h"

#include <string.h>

#include "esp_timer.h"

namespace swmic {

void TriggerGate::on_button(bool down) {
  if (down == key_down_) return;
  key_down_ = down;
  if (down) {
    press_pending_ = true;
    sink_.trigger(true);
  }
}

void TriggerGate::on_chunk(const int16_t *samples) {
  const int64_t now = esp_timer_get_time();
  const bool was_active = active_;
  const bool held = key_down_ || press_pending_;
  press_pending_ = false;

  if (held) {
    release_at_ = 0;
  } else if (active_ && release_at_ == 0) {
    release_at_ = now + RELEASE_TAIL_US;
  }
  active_ = held || (active_ && now < release_at_);

  if (active_) {
    if (!was_active) {
      sink_.begin_utterance();
      for (int i = 0; i < preroll_count_; ++i) {
        sink_.audio(preroll_[(preroll_head_ + i) % PREROLL_CHUNKS], true);
      }
      preroll_count_ = 0;
    }
    sink_.audio(samples, true);
    return;
  }

  if (was_active) {  // trigger up after the tail, twice against loss
    sink_.trigger(false);
    sink_.trigger(false);
  }
  int slot = (preroll_head_ + preroll_count_) % PREROLL_CHUNKS;
  memcpy(preroll_[slot], samples, CHUNK_BYTES);
  if (preroll_count_ < PREROLL_CHUNKS) {
    ++preroll_count_;
  } else {
    preroll_head_ = (preroll_head_ + 1) % PREROLL_CHUNKS;
  }
}

}  // namespace swmic
