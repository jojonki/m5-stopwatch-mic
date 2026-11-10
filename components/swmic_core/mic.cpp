#include "mic.h"

#include <M5Unified.h>
#include <math.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "ui.h"

namespace swmic {
namespace {

constexpr int NUM_BUFS = 4;
constexpr int TASK_PRIORITY = 11;
constexpr int TASK_CORE = 1;

int16_t s_bufs[NUM_BUFS][CHUNK_SAMPLES];
QueueHandle_t s_filled_q;
ChunkFn s_fn;
void *s_ctx;

void release_cb(void *, void *data, size_t) {
  int16_t *p = static_cast<int16_t *>(data);
  xQueueSend(s_filled_q, &p, 0);
}

// peak level for the UI meter: -60..0 dBFS -> 0..1
float level_of(const int16_t *s) {
  int peak = 1;
  for (size_t i = 0; i < CHUNK_SAMPLES; ++i) {
    int v = abs(s[i]);
    if (v > peak) peak = v;
  }
  float db = 20.0f * log10f(peak / 32768.0f);
  return fminf(fmaxf((db + 60.0f) / 60.0f, 0.0f), 1.0f);
}

void task(void *) {
  M5.Mic.setBufferReleaseCallback(nullptr, release_cb);

  // The capture queue holds two requests; keep it full so capture is gapless.
  int next = 0;
  for (int i = 0; i < 2; ++i) {
    M5.Mic.record(s_bufs[next], CHUNK_SAMPLES, SAMPLE_RATE);
    next = (next + 1) % NUM_BUFS;
  }

  for (;;) {
    int16_t *done = nullptr;
    if (xQueueReceive(s_filled_q, &done, portMAX_DELAY) != pdTRUE) continue;
    while (!M5.Mic.record(s_bufs[next], CHUNK_SAMPLES, SAMPLE_RATE)) {
      vTaskDelay(1);
    }
    next = (next + 1) % NUM_BUFS;

    g_ui.level.store(level_of(done));
    s_fn(done, s_ctx);
  }
}

}  // namespace

void mic_start(ChunkFn fn, void *ctx) {
  s_fn = fn;
  s_ctx = ctx;

  auto cfg = M5.Mic.config();
  cfg.sample_rate = SAMPLE_RATE;
  cfg.task_priority = 10;
  M5.Mic.config(cfg);
  M5.Mic.begin();

  s_filled_q = xQueueCreate(NUM_BUFS, sizeof(int16_t *));
  xTaskCreatePinnedToCore(task, "mic", 6144, nullptr, TASK_PRIORITY, nullptr, TASK_CORE);
}

}  // namespace swmic
