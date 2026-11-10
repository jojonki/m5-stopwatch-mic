// M5Stack StopWatch: USB microphone (UAC2) + push-to-hold key (HID keyboard)
//
//  - Built-in MEMS mic (ES8311) is streamed to the host as a 16 kHz mono USB mic.
//  - While KEYA (yellow) is held, the host sees PUSH_KEY held down.

#include <M5Unified.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_private/usb_phy.h"
#include "esp_log.h"

#include "tusb.h"
#include "usb_descriptors.h"
#include "mic.h"
#include "ui.h"

static const char *TAG = "swmic";

// Key sent while the button is held (HID usage ID)
static constexpr uint8_t PUSH_KEY = HID_KEY_F3;

static constexpr uint32_t SAMPLE_RATE = CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE;
static_assert(SAMPLE_RATE == swmic::SAMPLE_RATE, "USB descriptor rate must match the mic");

static volatile bool s_streaming = false;  // host opened the mic stream
static volatile bool s_muted = false;

//--------------------------------------------------------------------
// USB
//--------------------------------------------------------------------
static void usb_phy_init(void) {
  static usb_phy_handle_t phy_hdl;
  usb_phy_config_t phy_conf = {};
  phy_conf.controller = USB_PHY_CTRL_OTG;
  phy_conf.target = USB_PHY_TARGET_INT;
  phy_conf.otg_mode = USB_OTG_MODE_DEVICE;
  phy_conf.otg_speed = USB_PHY_SPEED_FULL;
  ESP_ERROR_CHECK(usb_new_phy(&phy_conf, &phy_hdl));
}

static void usb_device_task(void *) {
  tusb_rhport_init_t dev_init = {};
  dev_init.role = TUSB_ROLE_DEVICE;
  dev_init.speed = TUSB_SPEED_FULL;
  tusb_init(0, &dev_init);
  for (;;) {
    tud_task();  // blocks until there is an event
  }
}

//--------------------------------------------------------------------
// Microphone -> USB
//--------------------------------------------------------------------
static void on_mic_chunk(const int16_t *samples, void *) {
  if (!s_streaming) return;
  if (s_muted) {
    static const int16_t silence[swmic::CHUNK_SAMPLES] = {};
    samples = silence;
  }
  tud_audio_write(samples, swmic::CHUNK_BYTES);
}

//--------------------------------------------------------------------
// UAC2 control requests
//--------------------------------------------------------------------
static int16_t s_volume = 0;  // 1/256 dB units, reported only
static uint32_t s_samp_freq = SAMPLE_RATE;
static uint8_t s_clk_valid = 1;

extern "C" bool tud_audio_set_itf_cb(uint8_t, tusb_control_request_t const *p_request) {
  uint8_t itf = tu_u16_low(p_request->wIndex);
  uint8_t alt = tu_u16_low(p_request->wValue);
  if (itf == ITF_NUM_AUDIO_STREAMING) {
    s_streaming = (alt != 0);
    ESP_LOGI(TAG, "stream %s", s_streaming ? "start" : "stop");
  }
  return true;
}

extern "C" bool tud_audio_set_itf_close_ep_cb(uint8_t, tusb_control_request_t const *p_request) {
  if (tu_u16_low(p_request->wIndex) == ITF_NUM_AUDIO_STREAMING) {
    s_streaming = false;
  }
  return true;
}

extern "C" bool tud_audio_set_req_entity_cb(uint8_t, tusb_control_request_t const *p_request, uint8_t *buf) {
  uint8_t ctrl_sel = tu_u16_high(p_request->wValue);
  uint8_t entity = tu_u16_high(p_request->wIndex);
  TU_VERIFY(p_request->bRequest == AUDIO20_CS_REQ_CUR);

  if (entity == UAC2_ENTITY_FEATURE_UNIT) {
    if (ctrl_sel == AUDIO20_FU_CTRL_MUTE) {
      s_muted = ((audio20_control_cur_1_t *) buf)->bCur;
      return true;
    }
    if (ctrl_sel == AUDIO20_FU_CTRL_VOLUME) {
      s_volume = (int16_t) ((audio20_control_cur_2_t *) buf)->bCur;
      return true;
    }
  }
  return false;
}

extern "C" bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
  uint8_t ctrl_sel = tu_u16_high(p_request->wValue);
  uint8_t entity = tu_u16_high(p_request->wIndex);

  if (entity == UAC2_ENTITY_INPUT_TERMINAL && ctrl_sel == AUDIO20_TE_CTRL_CONNECTOR) {
    audio20_desc_channel_cluster_t ret = {};
    ret.bNrChannels = 1;
    return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &ret, sizeof(ret));
  }

  if (entity == UAC2_ENTITY_FEATURE_UNIT) {
    if (ctrl_sel == AUDIO20_FU_CTRL_MUTE) {
      uint8_t m = s_muted;
      return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &m, 1);
    }
    if (ctrl_sel == AUDIO20_FU_CTRL_VOLUME) {
      if (p_request->bRequest == AUDIO20_CS_REQ_CUR) {
        return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &s_volume, sizeof(s_volume));
      }
      if (p_request->bRequest == AUDIO20_CS_REQ_RANGE) {
        audio20_control_range_2_n_t(1) rng;
        rng.wNumSubRanges = 1;
        rng.subrange[0].bMin = 0;
        rng.subrange[0].bMax = 0;
        rng.subrange[0].bRes = 256;
        return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &rng, sizeof(rng));
      }
    }
  }

  if (entity == UAC2_ENTITY_CLOCK) {
    if (ctrl_sel == AUDIO20_CS_CTRL_SAM_FREQ) {
      if (p_request->bRequest == AUDIO20_CS_REQ_CUR) {
        return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &s_samp_freq, sizeof(s_samp_freq));
      }
      if (p_request->bRequest == AUDIO20_CS_REQ_RANGE) {
        audio20_control_range_4_n_t(1) rng;
        rng.wNumSubRanges = 1;
        rng.subrange[0].bMin = SAMPLE_RATE;
        rng.subrange[0].bMax = SAMPLE_RATE;
        rng.subrange[0].bRes = 0;
        return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &rng, sizeof(rng));
      }
    }
    if (ctrl_sel == AUDIO20_CS_CTRL_CLK_VALID) {
      return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &s_clk_valid, 1);
    }
  }
  return false;
}

//--------------------------------------------------------------------
// HID callbacks (unused but required)
//--------------------------------------------------------------------
extern "C" uint16_t tud_hid_get_report_cb(uint8_t, uint8_t, hid_report_type_t, uint8_t *, uint16_t) {
  return 0;
}
extern "C" void tud_hid_set_report_cb(uint8_t, uint8_t, hid_report_type_t, uint8_t const *, uint16_t) {}

//--------------------------------------------------------------------
// Main
//--------------------------------------------------------------------
extern "C" void app_main(void) {
  auto cfg = M5.config();
  cfg.internal_mic = true;
  cfg.internal_spk = false;
  M5.begin(cfg);
  M5.Speaker.end();

  usb_phy_init();
  xTaskCreatePinnedToCore(usb_device_task, "tud", 4096, nullptr, 12, nullptr, 0);
  swmic::mic_start(on_mic_chunk, nullptr);

  ui_start();

  bool key_down = false;      // desired state
  bool key_sent = false;      // state the host last received

  for (;;) {
    M5.update();
    key_down = M5.BtnA.isPressed();

    if (key_down != key_sent && tud_mounted()) {
      if (tud_suspended()) {
        tud_remote_wakeup();
      } else if (tud_hid_ready()) {
        uint8_t keycodes[6] = {0};
        if (key_down) {
          keycodes[0] = PUSH_KEY;
        }
        if (tud_hid_keyboard_report(0, 0, keycodes)) {
          key_sent = key_down;
        }
      }
    }

    g_ui.key_down.store(key_sent);
    g_ui.mounted.store(tud_mounted());
    g_ui.streaming.store(s_streaming);

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
