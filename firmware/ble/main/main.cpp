// M5Stack StopWatch: wireless mic + push-to-hold trigger over BLE
//
// One GATT notify characteristic (see ble_audio.h) carries:
//   audio packet : u8 flags | u8 seq | i16le predictor | u8 step_index | 160 B IMA ADPCM
//                  (20 ms of 16 kHz mono, low nibble first)
//   trigger-only : u8 flags | u8 seq
//   flags bit0 = trigger held
// Audio is sent only while the trigger is held (see swmic::TriggerGate).

#include <M5Unified.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "adpcm.h"
#include "ble_audio.h"
#include "mic.h"
#include "trigger_gate.h"
#include "ui.h"

static constexpr size_t AUDIO_HDR = 5;
static constexpr size_t AUDIO_PKT = AUDIO_HDR + swmic::CHUNK_SAMPLES / 2;

//--------------------------------------------------------------------
// Push-to-talk -> BLE notifications
//--------------------------------------------------------------------
struct BleSink : swmic::TriggerGate::Sink {
  SemaphoreHandle_t lock = xSemaphoreCreateMutex();
  uint8_t seq = 0;
  AdpcmState adpcm;

  void begin_utterance() override {
    adpcm = AdpcmState{};
  }

  void audio(const int16_t *samples, bool trigger) override {
    uint8_t pkt[AUDIO_PKT];
    xSemaphoreTake(lock, portMAX_DELAY);
    pkt[0] = trigger ? 1 : 0;
    pkt[1] = seq++;
    pkt[2] = (uint8_t) (adpcm.predictor & 0xFF);
    pkt[3] = (uint8_t) ((uint16_t) adpcm.predictor >> 8);
    pkt[4] = adpcm.index;
    adpcm_encode(adpcm, samples, swmic::CHUNK_SAMPLES, pkt + AUDIO_HDR);
    ble_audio_send(pkt, sizeof(pkt));
    xSemaphoreGive(lock);
  }

  void trigger(bool down) override {
    xSemaphoreTake(lock, portMAX_DELAY);
    uint8_t pkt[2] = {(uint8_t) (down ? 1 : 0), seq++};
    ble_audio_send(pkt, sizeof(pkt));
    xSemaphoreGive(lock);
  }
};

static BleSink *s_sink;
static swmic::TriggerGate *s_gate;

static void on_mic_chunk(const int16_t *samples, void *) {
  s_gate->on_chunk(samples);
}

//--------------------------------------------------------------------
// Main
//--------------------------------------------------------------------
extern "C" void app_main(void) {
  auto cfg = M5.config();
  cfg.internal_mic = true;
  cfg.internal_spk = false;
  M5.begin(cfg);
  M5.Speaker.end();

  ui_start();

  s_sink = new BleSink();
  s_gate = new swmic::TriggerGate(*s_sink);

  ble_audio_init("StopWatch Mic");
  swmic::mic_start(on_mic_chunk, nullptr);

  for (;;) {
    M5.update();
    bool down = M5.BtnA.isPressed();
    g_ui.key_down.store(down);
    s_gate->on_button(down);
    g_ui.mounted.store(ble_audio_connected());
    g_ui.streaming.store(ble_audio_subscribed());
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
