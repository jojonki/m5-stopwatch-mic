#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// GATT service exposing one notify characteristic that carries the audio /
// trigger packets. UUIDs (must match tools/swmic_helper.py):
//   service  7a1e0001-5c3b-4c8e-9a51-2f6d8e3b9c10
//   packets  7a1e0002-5c3b-4c8e-9a51-2f6d8e3b9c10

void ble_audio_init(const char *device_name);

// true while a central is connected / has enabled notifications
bool ble_audio_connected(void);
bool ble_audio_subscribed(void);

// Send one packet as a notification. Returns false if not subscribed,
// the packet exceeds the negotiated MTU, or the stack is out of buffers.
bool ble_audio_send(const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif
