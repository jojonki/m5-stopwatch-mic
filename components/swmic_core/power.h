#pragma once
// Battery-oriented power management for the wireless firmwares.

namespace swmic {

// Enables dynamic frequency scaling (CPU drops to 80 MHz when idle) and
// automatic light sleep. Light sleep only happens when no driver holds a PM
// lock; the always-on mic (needed for pre-roll) holds one, so in practice the
// gain comes from DFS. Requires CONFIG_PM_ENABLE; a no-op otherwise.
// Not for the USB firmware: USB OTG must keep its clocks running.
void power_save_init();

}  // namespace swmic
