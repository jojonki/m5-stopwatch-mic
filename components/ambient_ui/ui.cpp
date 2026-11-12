// Ambient UI for the round AMOLED.
//
//  idle    : true black, one faint thin ring in the middle
//  trigger : the ring softly fades into a glowing orb that breathes with the voice
//  untouched for 5 s : the panel goes to sleep until the next press
//                      (not while on USB power: then it stays on)
//  while the panel is on : a small battery gauge sits above the orb
//
// Only a 240x240 region around the centre is redrawn, motion is slow
// (fades and low-passed size changes), and unchanged frames are skipped,
// so the low panel refresh rate never shows.

#include "ui.h"

#include <M5Unified.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

UiState g_ui;

namespace {
portMUX_TYPE s_peer_mux = portMUX_INITIALIZER_UNLOCKED;
char s_peer[64] = "";
uint32_t s_peer_seq = 0;
}  // namespace

void ui_set_peer(const char *name) {
  if (!name) name = "";
  portENTER_CRITICAL(&s_peer_mux);
  if (strncmp(s_peer, name, sizeof(s_peer) - 1) != 0) {
    strlcpy(s_peer, name, sizeof(s_peer));
    ++s_peer_seq;
  }
  portEXIT_CRITICAL(&s_peer_mux);
}

namespace {

constexpr int ORB_BOX = 240;            // redraw region (square)
constexpr int RING_R = 64;              // idle ring radius
constexpr uint32_t RING_COLOR = 0x3A3A44;
constexpr uint32_t GLOW_COLOR = 0x9FE8FF;  // pale ice blue
constexpr uint32_t CORE_COLOR = 0xF2FCFF;
constexpr TickType_t SLEEP_AFTER = pdMS_TO_TICKS(5 * 1000);  // screen off when untouched

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

// tiny connection dot under the orb, and the host name (or "Not connected") under it
void render_status(bool mounted, bool streaming, const char *peer) {
  uint32_t col = !mounted ? 0x401818 : (streaming ? 0x1C4A34 : 0x2A2A30);
  M5.Display.fillSmoothCircle(s_cx, s_cy + 150, 4, rgb(col));

  constexpr int W = 300, H = 26, Y = 178;
  M5.Display.fillRect(s_cx - W / 2, s_cy + Y - H / 2, W, H, TFT_BLACK);
  M5.Display.setFont(&fonts::lgfxJapanGothicP_20);
  M5.Display.setTextDatum(middle_center);
  if (peer[0]) {
    M5.Display.setTextColor(rgb(0x6A6A78), TFT_BLACK);
    M5.Display.drawString(peer, s_cx, s_cy + Y);
  } else {
    M5.Display.setTextColor(rgb(0x3A3A44), TFT_BLACK);
    M5.Display.drawString("Not connected", s_cx, s_cy + Y);
  }
}

// small battery gauge above the orb: [bolt] outline + fill + "82%".
// level < 0 = unknown. plugged = USB power present (bolt shown),
// charging = the charger is actively charging (bolt / fill / text go green).
void render_battery(int level, bool plugged, bool charging) {
  constexpr int W = 180, H = 26, Y = -178;
  M5.Display.fillRect(s_cx - W / 2, s_cy + Y - H / 2, W, H, TFT_BLACK);
  if (level < 0) return;
  if (level > 100) level = 100;

  const uint32_t GREEN = 0x2E9A62;
  const bool low = level <= 15 && !plugged;
  const uint16_t line = rgb(low ? 0x7A3434 : 0x4A4A56);
  const uint16_t fill = rgb(charging ? GREEN : (low ? 0x9A3A3A : 0x6A6A78));

  char text[8];
  snprintf(text, sizeof(text), "%d%%", level);
  M5.Display.setFont(&fonts::lgfxJapanGothicP_20);
  M5.Display.setTextDatum(middle_left);
  const int tw = M5.Display.textWidth(text);

  constexpr int BOLT_W = 10, BOLT_H = 16, BW = 28, BH = 14, GAP = 8;
  const int total = (plugged ? BOLT_W + 6 : 0) + BW + 3 + GAP + tw;
  int x = s_cx - total / 2;
  const int yc = s_cy + Y;

  if (plugged) {
    const uint16_t bc = rgb(charging ? GREEN : 0x6A6A78);
    const int bx = x, by = yc - BOLT_H / 2;
    M5.Display.fillTriangle(bx + 7, by, bx + 1, by + 9, bx + 6, by + 9, bc);
    M5.Display.fillTriangle(bx + 4, by + 7, bx + 9, by + 7, bx + 3, by + BOLT_H, bc);
    x += BOLT_W + 6;
  }

  const int y0 = yc - BH / 2;
  M5.Display.drawRoundRect(x, y0, BW, BH, 3, line);
  M5.Display.fillRect(x + BW, yc - 3, 3, 6, line);  // terminal nub
  const int fw = (BW - 4) * level / 100;
  if (fw > 0) M5.Display.fillRect(x + 2, y0 + 2, fw, BH - 4, fill);

  M5.Display.setTextColor(charging ? fill : rgb(0x6A6A78), TFT_BLACK);
  M5.Display.drawString(text, x + BW + 3 + GAP, yc + 1);
}

// USB (5V in) present, read from the StopWatch's PM1.
bool usb_power_present() {
  if (M5.Power.getType() != m5::Power_Class::pmic_t::pmic_m5pm1) return false;
  m5::M5PM1_Class::pwr_src_t src;
  if (!M5.Power.M5pm1.getPowerSource(&src)) return false;
  return src & (m5::M5PM1_Class::vin | m5::M5PM1_Class::vinout);
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
  TickType_t last_touch = last;
  bool asleep = false;
  uint32_t shown_peer_seq = 0;
  char peer[64] = "";
  int last_batt = -2;                       // level*4 + plugged*2 + charging, -2 = not drawn
  TickType_t last_batt_read = 0;
  bool plugged_now = false;                 // USB power present -> never sleep
  constexpr TickType_t BATT_EVERY = pdMS_TO_TICKS(2 * 1000);  // quick to show plug / unplug

  for (;;) {
    bool key = g_ui.key_down.load();
    bool mounted = g_ui.mounted.load();
    bool streaming = g_ui.streaming.load();

    // host name changes count as activity so the new name is shown
    bool peer_changed = false;
    portENTER_CRITICAL(&s_peer_mux);
    if (s_peer_seq != shown_peer_seq) {
      shown_peer_seq = s_peer_seq;
      strlcpy(peer, s_peer, sizeof(peer));
      peer_changed = true;
    }
    portEXIT_CRITICAL(&s_peer_mux);

    // Screen sleep: off after SLEEP_AFTER without a press, back on at the next press.
    TickType_t now = xTaskGetTickCount();
    if (key || peer_changed) last_touch = now;
    if (peer_changed) last_status = -1;
    if (asleep) {
      // plugging in wakes the panel (it then stays on while powered)
      if (now - last_batt_read > BATT_EVERY) {
        last_batt_read = now;
        plugged_now = usb_power_present();
      }
      if (!key && !peer_changed && !plugged_now) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(50));
        continue;
      }
      asleep = false;
      M5.Display.wakeup();
      M5.Display.fillScreen(TFT_BLACK);
      on = 0;
      last_q_on = last_q_lv = last_status = -1;  // force a full redraw
      last_batt = -2;
    } else if (!plugged_now && now - last_touch > SLEEP_AFTER && on < 0.01f) {
      asleep = true;
      M5.Display.fillScreen(TFT_BLACK);
      M5.Display.sleep();
      continue;
    }

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
      render_status(mounted, streaming, peer);
    }

    // battery: read on wake and every 2 s while the panel is on
    if (last_batt == -2 || now - last_batt_read > BATT_EVERY) {
      last_batt_read = now;
      int level = M5.Power.getBatteryLevel();
      bool charging = level >= 0 && M5.Power.isCharging();
      bool plugged = charging || usb_power_present();
      if (plugged_now && !plugged) last_touch = now;  // unplugged: sleep 5 s from now
      plugged_now = plugged;
      int b = level < 0 ? -1 : level * 4 + (plugged ? 2 : 0) + (charging ? 1 : 0);
      if (b != last_batt) {
        last_batt = b;
        render_battery(level, plugged, charging);
      }
    }

    vTaskDelayUntil(&last, pdMS_TO_TICKS(20));
  }
}

}  // namespace

void ui_start(void) {
  xTaskCreatePinnedToCore(ui_task, "ui", 4096, nullptr, 3, nullptr, 1);
}
