# m5-stopwatch-mic

Turn an [M5Stack StopWatch](https://docs.m5stack.com/en/core/StopWatch) into a
push-to-talk microphone for your Mac.

- **Microphone**: the built-in mic, 16 kHz mono.
- **Trigger key**: hold the yellow button (KEYA) and the Mac sees **F3** held
  down; release it and F3 is released. Use it as a push-to-talk / dictation
  hotkey.
- **Ambient display**: a faint ring at rest, which turns into a glowing orb
  that follows your voice while the button is held. A small dot shows the link
  state (red: not connected, grey: connected, green: streaming), with the
  connected computer's name (or "Not connected") under it. The screen turns
  off after 5 s without a press.

Three firmwares, pick the one that fits:

| | USB | Wi-Fi | BLE |
|---|---|---|---|
| Directory | `firmware/usb` | `firmware/wifi` | `firmware/ble` |
| Connection | USB-C cable | same LAN | Bluetooth LE |
| Host software | none | helper + BlackHole | tray app (or helper) + BlackHole / VB-CABLE |
| Shows up as | USB mic "StopWatch Mic" + USB keyboard | "BlackHole 2ch" input | "BlackHole 2ch" input |
| Audio sent | always while an app uses the mic | only while the button is held | only while the button is held |

The wireless versions send ~100 ms of audio from just before the press and keep
the key held ~300 ms after release, so words are not cut at either end.

## Requirements

- M5Stack StopWatch (ESP32-S3)
- [ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/get-started/)
  (default location `~/esp/esp-idf`, or set `IDF_PATH`)
- Wireless versions only:
  - [uv](https://docs.astral.sh/uv/)
  - [BlackHole 2ch](https://existential.audio/blackhole/): `brew install --cask blackhole-2ch`

## Build & flash

```sh
. ./idf_env.sh
cd firmware/usb           # or firmware/wifi / firmware/ble
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash
```

To flash a device that is running the **USB** firmware, put it in download
mode first: hold the power button until the green LED blinks. After flashing,
short-press the power button to boot. The Wi-Fi and BLE firmwares can be
re-flashed directly.

## USB version

Plug it in. The Mac sees a USB microphone named **StopWatch Mic** and a USB
keyboard. No other software is needed.

## Wi-Fi version

1. Set your Wi-Fi network (2.4 GHz):
   ```sh
   cp firmware/wifi/main/wifi_secrets.h.example firmware/wifi/main/wifi_secrets.h
   # edit SWMIC_WIFI_SSID / SWMIC_WIFI_PASSWORD
   ```
2. Build & flash `firmware/wifi`.
3. On the Mac (same network):
   ```sh
   uv run tools/swmic_helper.py
   ```
4. In your app, choose **BlackHole 2ch** as the microphone.

The device finds the helper automatically (UDP broadcast, ports 4999/5000).
A VPN that captures all traffic, or a network that isolates clients, will
block it; use the USB or BLE version there.

## BLE version

1. Build & flash `firmware/ble`.
2. On the Mac:
   ```sh
   uv run tools/swmic_helper.py --transport ble
   ```
3. In your app, choose **BlackHole 2ch** as the microphone.

No pairing or network setup is needed; the helper finds the device by its
BLE service.

## Tray app for BLE (`apps/swmic-bridge`, macOS / Windows)

A small native app (~1 MB) that does the helper's job for the BLE firmware
and lives in the menu bar / system tray. The dot shows the state: grey =
searching, green = connected, red = button held.

It plays the audio into a virtual audio device and holds F3 while the button
is held:

| | macOS | Windows |
|---|---|---|
| Virtual device | [BlackHole 2ch](https://existential.audio/blackhole/) | [VB-CABLE](https://vb-audio.com/Cable/) |
| Mic to choose in your app | BlackHole 2ch | CABLE Output |

Download from [Releases](https://github.com/jojonki/m5-stopwatch-mic/releases):

- **macOS** (Apple silicon + Intel): `StopWatch-Mic-macOS.zip`. The app is
  not notarized; on first launch right-click it and choose **Open**, or run
  `xattr -dr com.apple.quarantine "StopWatch Mic.app"`.
- **Windows** (x64): `swmic-bridge-windows-x64.zip`. SmartScreen may warn on
  first run (unsigned): **More info → Run anyway**.

The tray menu has **Disconnect / Connect**: the StopWatch links to one
computer at a time, so disconnect on one machine and connect on the other to
switch. The StopWatch shows the connected computer's name under the dot.

Build (needs [Rust](https://rustup.rs/)):

```sh
# macOS: builds "StopWatch Mic.app" into apps/swmic-bridge/target/
apps/swmic-bridge/macos/bundle.sh

# Windows (on Windows, or cross-compiled from macOS with mingw-w64)
cd apps/swmic-bridge
cargo build --release                                   # on Windows
cargo build --release --target x86_64-pc-windows-gnu    # from macOS
```

Tray menu:

- **Trigger key**: F1-F20 (remembered across restarts).
- **Disconnect / Connect**: hand the StopWatch over to another computer.
- **Check for updates**: also checked at startup and daily; when a newer
  release exists the item becomes **Update to vX.Y.Z and restart**, which
  downloads it from GitHub Releases, replaces the app and restarts it. On
  macOS, re-enable the app under Accessibility after an update (the app is
  ad-hoc signed, so macOS sees each build as a new app).

Options: `--key f3` (overrides the menu choice), `--device "<output device name>"`.
On macOS, allow Bluetooth on first launch and give the app Accessibility
permission so the key reaches other apps.

## Helper (`tools/swmic_helper.py`)

Receives audio and button state from the wireless firmwares, plays the audio
into BlackHole and presses the trigger key.

```
--transport udp|ble   udp = Wi-Fi firmware (default), ble = BLE firmware
--key f3              key held while the button is held (f1 - f20)
--device BlackHole    output device (name substring)
--stats               print packet rate, level and buffer every 2 s
--record out.wav      also save the received audio
```

macOS permissions for the terminal that runs the helper:

- **Accessibility**: so the key events reach other apps.
- **Bluetooth** (BLE only): asked on first run.

## Layout

```
firmware/usb/           USB firmware (TinyUSB: UAC2 mic + HID keyboard)
firmware/wifi/          Wi-Fi firmware (UDP)
firmware/ble/           BLE firmware (NimBLE, IMA ADPCM)
components/swmic_core/  shared: mic capture, level meter, push-to-talk gating
components/ambient_ui/  shared: display UI
tools/swmic_helper.py   Mac helper for the wireless firmwares (Python)
apps/swmic-bridge/      tray app for the BLE firmware (Rust, macOS / Windows)
```

Each firmware only contains its transport; behavior shared by all of them
lives in `components/`, so a change there applies to every connection type.

## Customizing

- Trigger key: `PUSH_KEY` in `firmware/usb/main/main.cpp` (USB),
  `--key` option of the helper (wireless).
- Pre-roll / release tail: `PREROLL_CHUNKS`, `RELEASE_TAIL_US` in
  `components/swmic_core/trigger_gate.h` (wireless).
- UI colors / size: `GLOW_COLOR`, `RING_R` in `components/ambient_ui/ui.cpp`.
