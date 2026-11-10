#pragma once

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------
// Common
//--------------------------------------------------------------------
#define CFG_TUSB_OS               OPT_OS_FREERTOS
#define CFG_TUSB_OS_INC_PATH      freertos/
#define CFG_TUD_ENABLED           1
#define CFG_TUD_MAX_SPEED         OPT_MODE_FULL_SPEED
#define CFG_TUSB_DEBUG            0

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN        __attribute__((aligned(4)))

#define CFG_TUD_ENDPOINT0_SIZE    64

//--------------------------------------------------------------------
// Classes
//--------------------------------------------------------------------
#define CFG_TUD_AUDIO             1
#define CFG_TUD_HID               1
#define CFG_TUD_CDC               0
#define CFG_TUD_MSC               0
#define CFG_TUD_MIDI              0
#define CFG_TUD_VENDOR            0

//--------------------------------------------------------------------
// Audio (UAC2 microphone, 16 kHz / 16 bit / mono)
//--------------------------------------------------------------------
#define CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE              16000
#define CFG_TUD_AUDIO_ENABLE_EP_IN                    1
#define CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX    2
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX            1
#define CFG_TUD_AUDIO_EP_SZ_IN                        TUD_AUDIO_EP_SIZE(0, CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE, CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX, CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX)
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX             CFG_TUD_AUDIO_EP_SZ_IN
// Mic delivers 20 ms chunks; keep ~80 ms of headroom in the IN FIFO.
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ          (80 * CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE / 1000 * CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX)

//--------------------------------------------------------------------
// HID (boot keyboard)
//--------------------------------------------------------------------
#define CFG_TUD_HID_EP_BUFSIZE    8

#ifdef __cplusplus
}
#endif
