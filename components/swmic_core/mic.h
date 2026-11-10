#pragma once
// Continuous capture from the StopWatch's built-in mic (ES8311 via M5Unified).

#include <stddef.h>
#include <stdint.h>

namespace swmic {

constexpr uint32_t SAMPLE_RATE = 16000;
constexpr size_t   CHUNK_SAMPLES = SAMPLE_RATE / 50;  // 20 ms
constexpr size_t   CHUNK_BYTES = CHUNK_SAMPLES * sizeof(int16_t);

// Called from the capture task for every 20 ms chunk. `samples` is valid only
// during the call.
using ChunkFn = void (*)(const int16_t *samples, void *ctx);

// Configures and starts the mic, then runs a capture task that feeds the UI
// level meter (g_ui.level) and calls `fn` for each chunk. Call after M5.begin().
void mic_start(ChunkFn fn, void *ctx);

}  // namespace swmic
