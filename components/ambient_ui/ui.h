#pragma once

#include <atomic>

// State shared from the USB / mic / key logic to the UI task.
struct UiState {
  std::atomic<bool>  key_down{false};   // key state the host has received
  std::atomic<bool>  mounted{false};    // USB configured by host
  std::atomic<bool>  streaming{false};  // host is reading the mic
  std::atomic<float> level{0.0f};       // mic level, 0..1 (dB-scaled)
};

extern UiState g_ui;

void ui_start(void);
