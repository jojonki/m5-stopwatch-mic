// USB descriptors: composite UAC2 microphone + HID keyboard
#include <string.h>
#include "tusb.h"
#include "usb_descriptors.h"

//--------------------------------------------------------------------
// Device
//--------------------------------------------------------------------
static tusb_desc_device_t const desc_device = {
  .bLength            = sizeof(tusb_desc_device_t),
  .bDescriptorType    = TUSB_DESC_DEVICE,
  .bcdUSB             = 0x0200,
  // IAD is required for the audio function
  .bDeviceClass       = TUSB_CLASS_MISC,
  .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
  .bDeviceProtocol    = MISC_PROTOCOL_IAD,
  .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
  .idVendor           = 0x303A,  // Espressif
  .idProduct          = 0x4010,  // arbitrary test PID
  .bcdDevice          = 0x0100,
  .iManufacturer      = STRID_MANUFACTURER,
  .iProduct           = STRID_PRODUCT,
  .iSerialNumber      = STRID_SERIAL,
  .bNumConfigurations = 1,
};

uint8_t const *tud_descriptor_device_cb(void) {
  return (uint8_t const *) &desc_device;
}

//--------------------------------------------------------------------
// HID report (boot keyboard)
//--------------------------------------------------------------------
static uint8_t const desc_hid_report[] = {
  TUD_HID_REPORT_DESC_KEYBOARD()
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
  (void) instance;
  return desc_hid_report;
}

//--------------------------------------------------------------------
// Configuration
//--------------------------------------------------------------------
#define EPNUM_AUDIO_IN   0x81
#define EPNUM_HID_IN     0x82

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_AUDIO20_MIC_ONE_CH_DESC_LEN + TUD_HID_DESC_LEN)

static uint8_t const desc_configuration[] = {
  TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

  TUD_AUDIO20_MIC_ONE_CH_DESCRIPTOR(ITF_NUM_AUDIO_CONTROL, STRID_AUDIO,
                                    CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX,
                                    CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_TX * 8,
                                    EPNUM_AUDIO_IN, CFG_TUD_AUDIO_EP_SZ_IN),

  TUD_HID_DESCRIPTOR(ITF_NUM_HID, STRID_HID, HID_ITF_PROTOCOL_KEYBOARD,
                     sizeof(desc_hid_report), EPNUM_HID_IN, CFG_TUD_HID_EP_BUFSIZE, 1),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
  (void) index;
  return desc_configuration;
}

//--------------------------------------------------------------------
// Strings
//--------------------------------------------------------------------
static char const *string_desc_arr[] = {
  [STRID_LANGID]       = (const char[]) {0x09, 0x04},
  [STRID_MANUFACTURER] = "M5Stack",
  [STRID_PRODUCT]      = "StopWatch Mic",
  [STRID_SERIAL]       = "SW0001",
  [STRID_AUDIO]        = "StopWatch Mic",
  [STRID_HID]          = "StopWatch Keys",
};

static uint16_t _desc_str[32 + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  (void) langid;
  size_t chr_count;

  if (index == STRID_LANGID) {
    memcpy(&_desc_str[1], string_desc_arr[0], 2);
    chr_count = 1;
  } else {
    if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) {
      return NULL;
    }
    const char *str = string_desc_arr[index];
    chr_count = strlen(str);
    if (chr_count > 32) {
      chr_count = 32;
    }
    for (size_t i = 0; i < chr_count; i++) {
      _desc_str[1 + i] = str[i];
    }
  }

  _desc_str[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
  return _desc_str;
}
