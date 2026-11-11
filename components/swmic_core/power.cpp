#include "power.h"

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_pm.h"

namespace swmic {

void power_save_init() {
#if CONFIG_PM_ENABLE
  esp_pm_config_t cfg = {};
  cfg.max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
  cfg.min_freq_mhz = 80;  // keep APB at 80 MHz: M5GFX drives SPI registers directly
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
  cfg.light_sleep_enable = true;
#endif
  esp_err_t err = esp_pm_configure(&cfg);
  ESP_LOGI("power", "pm %d-%d MHz, light sleep %d: %s", cfg.min_freq_mhz, cfg.max_freq_mhz,
           cfg.light_sleep_enable, esp_err_to_name(err));
#endif
}

}  // namespace swmic
