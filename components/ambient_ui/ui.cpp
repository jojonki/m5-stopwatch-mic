// Ambient UI for the round AMOLED.
//
//  idle    : true black, one faint thin ring in the middle
//  trigger : the ring softly fades into a glowing orb that breathes with the voice
//
// Only a 240x240 region around the centre is redrawn, motion is slow
// (fades and low-passed size changes), and unchanged frames are skipped,
// so the low panel refresh rate never shows.

#include "ui.h"

#include <M5Unified.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

UiState g_ui;

namespace {

constexpr int ORB_BOX = 240;            // redraw region (square)
constexpr int RING_R = 64;              // idle ring radius
constexpr uint32_t RING_COLOR = 0x3A3A44;
constexpr uint32_t GLOW_COLOR = 0x9FE8FF;  // pale ice blue
constexpr uint32_t CORE_COLOR = 0xF2FCFF;

M5Canvas s_orb(&M5.Display);
int s_cx, s_cy;

uint16_t rgb(uint32_t c) {
  return lgfx::color565((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

// scale an RGB888 color towards black
uint32_t dim(uint32_t c, float k) {
  if (k <= 0) return 0;
  if (k > 1) k = 1;
  uint32_t r = ((c >> 16) & 0xFF) * k, g = ((c >> 8) & 0xFF) * k, b = (c & 0xFF) * k;
  return (r << 16) | (g << 8) | b;
}

uint32_t mix(uint32_t a, uint32_t b, float t) {
  auto ch = [&](int sh) {
    float va = (a >> sh) & 0xFF, vb = (b >> sh) & 0xFF;
    return (uint32_t) (va + (vb - va) * t) << sh;
  };
  return ch(16) | ch(8) | ch(0);
}

// on: 0..1 fade of the trigger state, lv: smoothed voice level 0..1
void render_orb(float on, float lv) {
  auto &c = s_orb;
  const int x = ORB_BOX / 2, y = ORB_BOX / 2;
  c.fillScreen(TFT_BLACK);

  // idle ring, fading out as the orb fades in
  float ring_k = 1.0f - on;
  if (ring_k > 0.01f) {
    c.fillSmoothCircle(x, y, RING_R + 1, rgb(dim(RING_COLOR, ring_k)));
    c.fillSmoothCircle(x, y, RING_R - 1, TFT_BLACK);
  }
  if (on < 0.01f) return;

  // glow: concentric discs, dim outside -> bright core
  const float radius = RING_R * (0.80f + 0.35f * lv);  // breathes with the voice
  const float glow = radius * (1.55f + 0.25f * lv);
  constexpr int LAYERS = 12;
  for (int i = 0; i < LAYERS; ++i) {
    float t = (float) i / (LAYERS - 1);          // 0 outer .. 1 inner
    float r = glow + (radius - glow) * t;
    float k = on * (0.04f + 0.30f * t * t);      // soft falloff
    c.fillCircle(x, y, (int) r, rgb(dim(GLOW_COLOR, k)));
  }
  // body and core
  c.fillSmoothCircle(x, y, radius, rgb(dim(GLOW_COLOR, on * (0.55f + 0.25f * lv))));
  c.fillSmoothCircle(x, y, radius * 0.62f, rgb(dim(mix(GLOW_COLOR, CORE_COLOR, 0.6f), on * (0.75f + 0.25f * lv))));
}

// tiny connection dot under the orb
void render_status(bool mounted, bool streaming) {
  uint32_t col = !mounted ? 0x401818 : (streaming ? 0x1C4A34 : 0x2A2A30);
  M5.Display.fillSmoothCircle(s_cx, s_cy + 150, 4, rgb(col));
}

void ui_task(void *) {
  s_cx = M5.Display.width() / 2;
  s_cy = M5.Display.height() / 2;
  M5.Display.fillScreen(TFT_BLACK);

  s_orb.setPsram(false);
  s_orb.setColorDepth(16);
  if (!s_orb.createSprite(ORB_BOX, ORB_BOX)) {
    s_orb.setPsram(true);
    s_orb.createSprite(ORB_BOX, ORB_BOX);
  }

  float on = 0, lv = 0;
  int last_q_on = -1, last_q_lv = -1;
  int last_status = -1;
  TickType_t last = xTaskGetTickCount();

  for (;;) {
    bool key = g_ui.key_down.load();
    bool mounted = g_ui.mounted.load();
    bool streaming = g_ui.streaming.load();

    // slow, frame-rate-forgiving easing
    on += ((key ? 1.0f : 0.0f) - on) * (key ? 0.22f : 0.12f);
    float target = key ? g_ui.level.load() : 0.0f;
    lv += (target - lv) * (target > lv ? 0.25f : 0.08f);

    // redraw only when the picture visibly changes
    int q_on = (int) (on * 64 + 0.5f), q_lv = (int) (lv * 48 + 0.5f);
    if (q_on != last_q_on || (q_on && q_lv != last_q_lv)) {
      last_q_on = q_on;
      last_q_lv = q_lv;
      render_orb(q_on / 64.0f, lv);
      s_orb.pushSprite(&M5.Display, s_cx - ORB_BOX / 2, s_cy - ORB_BOX / 2);
    }

    int status = (mounted ? 1 : 0) | (streaming ? 2 : 0);
    if (status != last_status) {
      last_status = status;
      render_status(mounted, streaming);
    }

    vTaskDelayUntil(&last, pdMS_TO_TICKS(20));
  }
}

}  // namespace

void ui_start(void) {
  xTaskCreatePinnedToCore(ui_task, "ui", 4096, nullptr, 3, nullptr, 1);
}
