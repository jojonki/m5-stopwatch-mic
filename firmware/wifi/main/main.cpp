// M5Stack StopWatch: wireless mic + push-to-hold trigger over Wi-Fi (UDP)
//
// Protocol (see tools/swmic_helper.py):
//   helper -> device  UDP :4999  "SWMIC-HELLO" + u16le reply_port   (broadcast, ~1 Hz)
//   device -> helper  UDP :reply_port
//       "SWM1" | u32le seq | u8 flags (bit0 = trigger held) | 3 x u8 reserved
//       | N x int16le samples (16 kHz mono; N = 320, or 0 for a trigger-only packet)
// Audio is sent only while the trigger is held (see swmic::TriggerGate).

#include <M5Unified.h>

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"

#include "mic.h"
#include "trigger_gate.h"
#include "ui.h"

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#error "Copy main/wifi_secrets.h.example to main/wifi_secrets.h and set your Wi-Fi credentials"
#endif

static const char *TAG = "swmic-wifi";

static constexpr uint16_t BEACON_PORT = 4999;
static constexpr char     BEACON_MAGIC[] = "SWMIC-HELLO";
static constexpr int64_t  HELPER_TIMEOUT_US = 3 * 1000 * 1000;


struct __attribute__((packed)) PacketHeader {
  char     magic[4];
  uint32_t seq;
  uint8_t  flags;
  uint8_t  reserved[3];
};


static EventGroupHandle_t s_wifi_events;
static constexpr EventBits_t WIFI_GOT_IP = BIT0;

// helper address (learned from its beacon)
static SemaphoreHandle_t s_tx_lock;
static int s_tx_sock = -1;
static sockaddr_in s_helper = {};
static volatile int64_t s_helper_seen_us = 0;
static uint32_t s_seq = 0;

static bool helper_alive(void) {
  return s_helper_seen_us && (esp_timer_get_time() - s_helper_seen_us) < HELPER_TIMEOUT_US;
}

//--------------------------------------------------------------------
// Wi-Fi
//--------------------------------------------------------------------
static void wifi_event_handler(void *, esp_event_base_t base, int32_t id, void *) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    xEventGroupClearBits(s_wifi_events, WIFI_GOT_IP);
    g_ui.mounted.store(false);
    ESP_LOGW(TAG, "Wi-Fi disconnected, retrying");
    esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    xEventGroupSetBits(s_wifi_events, WIFI_GOT_IP);
    g_ui.mounted.store(true);
    ESP_LOGI(TAG, "got IP");
  }
}

static void wifi_start(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  }
  s_wifi_events = xEventGroupCreate();
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, nullptr));

  wifi_config_t cfg = {};
  strlcpy((char *) cfg.sta.ssid, SWMIC_WIFI_SSID, sizeof(cfg.sta.ssid));
  strlcpy((char *) cfg.sta.password, SWMIC_WIFI_PASSWORD, sizeof(cfg.sta.password));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
  ESP_ERROR_CHECK(esp_wifi_start());
  // modem sleep adds 100 ms-class latency spikes to a realtime stream
  esp_wifi_set_ps(WIFI_PS_NONE);
}

//--------------------------------------------------------------------
// UDP
//--------------------------------------------------------------------
static void send_packet(const int16_t *samples, size_t n, bool trigger) {
  if (!helper_alive()) return;
  uint8_t buf[sizeof(PacketHeader) + swmic::CHUNK_BYTES];
  auto *h = reinterpret_cast<PacketHeader *>(buf);
  memcpy(h->magic, "SWM1", 4);
  h->flags = trigger ? 1 : 0;
  memset(h->reserved, 0, sizeof(h->reserved));
  if (n) memcpy(buf + sizeof(PacketHeader), samples, n * sizeof(int16_t));

  xSemaphoreTake(s_tx_lock, portMAX_DELAY);
  h->seq = s_seq++;
  sendto(s_tx_sock, buf, sizeof(PacketHeader) + n * sizeof(int16_t), 0,
         (const sockaddr *) &s_helper, sizeof(s_helper));
  xSemaphoreGive(s_tx_lock);
}

// Listens for helper beacons and remembers where to stream.
static void beacon_task(void *) {
  xEventGroupWaitBits(s_wifi_events, WIFI_GOT_IP, pdFALSE, pdTRUE, portMAX_DELAY);

  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(BEACON_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  bind(sock, (sockaddr *) &addr, sizeof(addr));

  timeval tv = {1, 0};
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  char buf[64];
  for (;;) {
    sockaddr_in from = {};
    socklen_t from_len = sizeof(from);
    int n = recvfrom(sock, buf, sizeof(buf), 0, (sockaddr *) &from, &from_len);
    const size_t mlen = sizeof(BEACON_MAGIC) - 1;
    if (n >= (int) (mlen + 2) && memcmp(buf, BEACON_MAGIC, mlen) == 0) {
      uint16_t port = (uint8_t) buf[mlen] | ((uint8_t) buf[mlen + 1] << 8);
      bool changed = s_helper.sin_addr.s_addr != from.sin_addr.s_addr || s_helper.sin_port != htons(port);
      xSemaphoreTake(s_tx_lock, portMAX_DELAY);
      s_helper.sin_family = AF_INET;
      s_helper.sin_addr = from.sin_addr;
      s_helper.sin_port = htons(port);
      xSemaphoreGive(s_tx_lock);
      s_helper_seen_us = esp_timer_get_time();
      if (changed) {
        ESP_LOGI(TAG, "helper at %s:%u", inet_ntoa(from.sin_addr), port);
      }
    }
    g_ui.streaming.store(helper_alive());
  }
}

//--------------------------------------------------------------------
// Push-to-talk -> UDP
//--------------------------------------------------------------------
struct UdpSink : swmic::TriggerGate::Sink {
  void audio(const int16_t *samples, bool trigger) override {
    send_packet(samples, swmic::CHUNK_SAMPLES, trigger);
  }
  void trigger(bool down) override {
    send_packet(nullptr, 0, down);
    if (down) send_packet(nullptr, 0, down);  // UDP: twice against a single loss
  }
};

static UdpSink s_sink;
static swmic::TriggerGate s_gate(s_sink);

static void on_mic_chunk(const int16_t *samples, void *) {
  s_gate.on_chunk(samples);
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

  s_tx_lock = xSemaphoreCreateMutex();
  wifi_start();  // brings up lwIP; sockets must be created after this
  s_tx_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  xTaskCreatePinnedToCore(beacon_task, "beacon", 4096, nullptr, 5, nullptr, 0);

  swmic::mic_start(on_mic_chunk, nullptr);

  for (;;) {
    M5.update();
    bool down = M5.BtnA.isPressed();
    g_ui.key_down.store(down);
    s_gate.on_button(down);
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
