// NimBLE peripheral: advertises the StopWatch Mic service and streams
// packets as GATT notifications.

#include "ble_audio.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble_audio";

// 7a1e0001-5c3b-4c8e-9a51-2f6d8e3b9c10 (little-endian byte order)
static const ble_uuid128_t SVC_UUID = BLE_UUID128_INIT(
    0x10, 0x9c, 0x3b, 0x8e, 0x6d, 0x2f, 0x51, 0x9a, 0x8e, 0x4c, 0x3b, 0x5c, 0x01, 0x00, 0x1e, 0x7a);
// 7a1e0002-5c3b-4c8e-9a51-2f6d8e3b9c10
static const ble_uuid128_t CHR_UUID = BLE_UUID128_INIT(
    0x10, 0x9c, 0x3b, 0x8e, 0x6d, 0x2f, 0x51, 0x9a, 0x8e, 0x4c, 0x3b, 0x5c, 0x02, 0x00, 0x1e, 0x7a);

static uint16_t s_chr_handle;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_subscribed = false;
static volatile uint16_t s_mtu = 23;
static uint8_t s_own_addr_type;
static const char *s_name = "StopWatch Mic";

static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
  // notify-only; a read returns nothing
  return 0;
}

static const struct ble_gatt_chr_def s_chrs[] = {
    {
        .uuid = &CHR_UUID.u,
        .access_cb = chr_access,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_chr_handle,
    },
    {0},
};

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = s_chrs,
    },
    {0},
};

static int gap_event(struct ble_gap_event *event, void *arg);

// Ask for a short interval so a press goes out within 15-30 ms, plus
// peripheral latency: with nothing to send we may skip up to 20 connection
// events (~0.3-0.6 s), so the radio idles while waiting for a press. When we
// do have data it goes out at the very next event, so latency is unaffected.
// (Apple limits: latency <= 30, interval * (latency + 1) <= 2 s.)
static esp_timer_handle_t s_param_timer;

static void request_low_power_params(void *arg) {
  uint16_t h = s_conn;
  if (h == BLE_HS_CONN_HANDLE_NONE) return;
  struct ble_gap_upd_params upd = {
      .itvl_min = 12,             // 15 ms
      .itvl_max = 24,             // 30 ms
      .latency = 20,
      .supervision_timeout = 400, // 4 s
  };
  int rc = ble_gap_update_params(h, &upd);
  if (rc) ESP_LOGW(TAG, "conn param request rc=%d", rc);
}

static void advertise(void) {
  struct ble_hs_adv_fields fields = {0};
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  fields.uuids128 = &SVC_UUID;
  fields.num_uuids128 = 1;
  fields.uuids128_is_complete = 1;
  int rc = ble_gap_adv_set_fields(&fields);
  if (rc) ESP_LOGE(TAG, "adv fields rc=%d", rc);

  struct ble_hs_adv_fields rsp = {0};
  rsp.name = (const uint8_t *) s_name;
  rsp.name_len = strlen(s_name);
  rsp.name_is_complete = 1;
  rc = ble_gap_adv_rsp_set_fields(&rsp);
  if (rc) ESP_LOGE(TAG, "scan rsp rc=%d", rc);

  struct ble_gap_adv_params params = {0};
  params.conn_mode = BLE_GAP_CONN_MODE_UND;
  params.disc_mode = BLE_GAP_DISC_MODE_GEN;
  params.itvl_min = BLE_GAP_ADV_ITVL_MS(100);
  params.itvl_max = BLE_GAP_ADV_ITVL_MS(150);
  rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
  if (rc && rc != BLE_HS_EALREADY) ESP_LOGE(TAG, "adv start rc=%d", rc);
}

static int gap_event(struct ble_gap_event *event, void *arg) {
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (event->connect.status == 0) {
        uint16_t h = event->connect.conn_handle;
        s_conn = h;
        ESP_LOGI(TAG, "connected");
        // Long PDUs so a 165 B packet fits in one air packet. (No 2M PHY
        // request: it raced macOS's own PHY update and tripped a controller
        // assert; 1M PHY has plenty of room for 64 kbps.)
        ble_hs_hci_util_set_data_len(h, 251, 2120);
        // macOS tends to refuse parameter requests made during discovery.
        esp_timer_start_once(s_param_timer, 5 * 1000 * 1000);
      } else {
        advertise();
      }
      break;

    case BLE_GAP_EVENT_DISCONNECT:
      ESP_LOGI(TAG, "disconnected (reason %d)", event->disconnect.reason);
      esp_timer_stop(s_param_timer);
      s_conn = BLE_HS_CONN_HANDLE_NONE;
      s_subscribed = false;
      s_mtu = 23;
      advertise();
      break;

    case BLE_GAP_EVENT_SUBSCRIBE:
      if (event->subscribe.attr_handle == s_chr_handle) {
        s_subscribed = event->subscribe.cur_notify;
        ESP_LOGI(TAG, "notify %s", s_subscribed ? "on" : "off");
      }
      break;

    case BLE_GAP_EVENT_MTU:
      s_mtu = event->mtu.value;
      ESP_LOGI(TAG, "mtu %u", event->mtu.value);
      break;

    case BLE_GAP_EVENT_CONN_UPDATE: {
      if (event->conn_update.status) {
        ESP_LOGW(TAG, "conn param update rejected (status %d)", event->conn_update.status);
        break;
      }
      struct ble_gap_conn_desc desc;
      if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
        ESP_LOGI(TAG, "conn interval %.2f ms, latency %u", desc.conn_itvl * 1.25f, desc.conn_latency);
      }
      break;
    }

    case BLE_GAP_EVENT_ADV_COMPLETE:
      advertise();
      break;

    default:
      break;
  }
  return 0;
}

static void on_sync(void) {
  ble_hs_util_ensure_addr(0);
  ble_hs_id_infer_auto(0, &s_own_addr_type);
  advertise();
}

static void on_reset(int reason) {
  ESP_LOGW(TAG, "host reset, reason %d", reason);
}

static void host_task(void *param) {
  nimble_port_run();
  nimble_port_freertos_deinit();
}

void ble_audio_init(const char *device_name) {
  if (device_name) s_name = device_name;

  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  }
  ESP_ERROR_CHECK(nimble_port_init());

  ble_hs_cfg.sync_cb = on_sync;
  ble_hs_cfg.reset_cb = on_reset;

  ble_svc_gap_init();
  ble_svc_gatt_init();
  ESP_ERROR_CHECK(ble_gatts_count_cfg(s_svcs));
  ESP_ERROR_CHECK(ble_gatts_add_svcs(s_svcs));
  ble_svc_gap_device_name_set(s_name);

  const esp_timer_create_args_t targs = {.callback = request_low_power_params, .name = "ble_params"};
  ESP_ERROR_CHECK(esp_timer_create(&targs, &s_param_timer));

  nimble_port_freertos_init(host_task);
}

bool ble_audio_connected(void) {
  return s_conn != BLE_HS_CONN_HANDLE_NONE;
}

bool ble_audio_subscribed(void) {
  return s_subscribed;
}

bool ble_audio_send(const uint8_t *data, uint16_t len) {
  uint16_t conn = s_conn;
  if (!s_subscribed || conn == BLE_HS_CONN_HANDLE_NONE) return false;
  if (len > s_mtu - 3) return false;
  struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
  if (!om) return false;
  return ble_gatts_notify_custom(conn, s_chr_handle, om) == 0;
}
