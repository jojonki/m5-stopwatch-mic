#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = [
#   "numpy",
#   "sounddevice",
#   "pyobjc-framework-Quartz",
#   "bleak",
# ]
# ///
"""Mac-side helper for the wireless StopWatch mic.

- Wi-Fi: broadcasts a beacon so the StopWatch finds this Mac (no IP config).
- BLE: scans for the StopWatch Mic service and subscribes to its packets.
- Plays the received 16 kHz mono stream into a virtual audio device
  (BlackHole by default); apps then pick that device as their microphone.
- Holds a key (F3 by default) while the StopWatch trigger is held.

Usage:
    uv run tools/swmic_helper.py                    # Wi-Fi firmware
    uv run tools/swmic_helper.py --transport ble    # BLE firmware
    options: [--device BlackHole] [--key f3] [--stats]

Needs: BlackHole (`brew install --cask blackhole-2ch`), and Accessibility
permission for the terminal running this (System Settings > Privacy &
Security > Accessibility) so the key events reach other apps.
"""

from __future__ import annotations

import argparse
import collections
import wave
import queue
import re
import socket
import subprocess
import struct
import threading
import time

import numpy as np
import sounddevice as sd
import Quartz

BEACON_PORT = 4999
BEACON_MAGIC = b"SWMIC-HELLO"
LISTEN_PORT = 5000
SAMPLE_RATE = 16000
CHUNK = 320  # 20 ms
HEADER = struct.Struct("<4sIB3x")
KEY_TIMEOUT_S = 0.5  # release the key if the device goes silent

# macOS virtual key codes
KEYCODES = {
    "f1": 122, "f2": 120, "f3": 99, "f4": 118, "f5": 96, "f6": 97,
    "f7": 98, "f8": 100, "f9": 101, "f10": 109, "f11": 103, "f12": 111,
    "f13": 105, "f14": 107, "f15": 113, "f16": 106, "f17": 64, "f18": 79,
    "f19": 80, "f20": 90,
}


class Key:
    """Synthetic key that looks like a hardware F-key to other apps."""

    def __init__(self, keycode: int):
        self.keycode = keycode
        self.down = False
        # HID-system source: the key also shows as held in the global key
        # state, which push-to-talk apps poll (CGEventSourceKeyState).
        self.source = Quartz.CGEventSourceCreate(Quartz.kCGEventSourceStateHIDSystemState)
        # Hardware F-keys carry the Fn flag; hotkey matchers may expect it.
        self.flags = Quartz.kCGEventFlagMaskSecondaryFn | Quartz.kCGEventFlagMaskNonCoalesced

    def set(self, down: bool) -> None:
        if down == self.down:
            return
        self.down = down
        ev = Quartz.CGEventCreateKeyboardEvent(self.source, self.keycode, down)
        Quartz.CGEventSetFlags(ev, self.flags)
        Quartz.CGEventPost(Quartz.kCGHIDEventTap, ev)
        print(time.strftime("%H:%M:%S"), "trigger", "DOWN" if down else "up", flush=True)


class JitterBuffer:
    """FIFO between the network and the audio callback, with latency control.

    Playback starts once `prime_ms` is buffered. Bursty links (BLE delivers
    several packets per connection event; the 100 ms pre-roll arrives at once)
    leave extra audio queued, which would otherwise persist as latency for the
    whole utterance. While the backlog is above `target_ms`, playback runs
    `catchup`x faster (linear resampling) until it is back on target: no audio
    is dropped and the slight speed-up is hard to notice in speech.
    """

    def __init__(self, prime_ms: int = 40, target_ms: int = 60, max_ms: int = 500, catchup: float = 1.1):
        ms = SAMPLE_RATE // 1000
        self.prime = prime_ms * ms
        self.target = target_ms * ms
        self.max = max_ms * ms
        self.catchup = catchup
        self.lock = threading.Lock()
        self.chunks: collections.deque[np.ndarray] = collections.deque()
        self.count = 0
        self.primed = False

    @property
    def backlog_ms(self) -> int:
        return self.count * 1000 // SAMPLE_RATE

    def push(self, samples: np.ndarray) -> None:
        with self.lock:
            self.chunks.append(samples)
            self.count += len(samples)
            while self.count > self.max:  # runaway (e.g. output stalled): drop oldest
                self.count -= len(self.chunks.popleft())

    def _take(self, m: int) -> np.ndarray:
        parts, need = [], m
        while need > 0 and self.chunks:
            c = self.chunks[0]
            if len(c) <= need:
                parts.append(self.chunks.popleft())
                need -= len(c)
            else:
                parts.append(c[:need])
                self.chunks[0] = c[need:]
                need = 0
        self.count -= m - need
        return np.concatenate(parts) if parts else np.zeros(0, dtype=np.float32)

    def pull(self, n: int) -> np.ndarray:
        with self.lock:
            if not self.primed:
                if self.count < self.prime:
                    return np.zeros(n, dtype=np.float32)
                self.primed = True
            if self.count < n:  # underrun: play what is left, then re-prime
                x = self._take(self.count)
                self.primed = False
                out = np.zeros(n, dtype=np.float32)
                out[: len(x)] = x
                return out
            m = n
            if self.count > self.target:
                m = min(int(n * self.catchup), self.count)
            x = self._take(m)
        if m == n:
            return x
        return np.interp(np.linspace(0, m - 1, n), np.arange(m), x).astype(np.float32)


def find_output_device(name: str) -> int:
    for i, d in enumerate(sd.query_devices()):
        if name.lower() in d["name"].lower() and d["max_output_channels"] > 0:
            return i
    raise SystemExit(
        f"Output device matching '{name}' not found. "
        "Install BlackHole: brew install --cask blackhole-2ch"
    )


def broadcast_addrs() -> list[str]:
    """Directed broadcast address of every IPv4 interface.

    macOS refuses the limited broadcast 255.255.255.255 ("No route to host")
    when several interfaces are up, so beacon each subnet explicitly.
    """
    out = subprocess.run(["ifconfig"], capture_output=True, text=True).stdout
    return sorted(set(re.findall(r"broadcast (\d+\.\d+\.\d+\.\d+)", out)))


def beacon_loop(stop: threading.Event) -> None:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    msg = BEACON_MAGIC + struct.pack("<H", LISTEN_PORT)
    addrs: list[str] = []
    tick = 0
    while not stop.is_set():
        if tick % 10 == 0:  # pick up network changes
            addrs = broadcast_addrs()
        tick += 1
        for a in addrs:
            try:
                s.sendto(msg, (a, BEACON_PORT))
            except OSError:
                pass
        stop.wait(1.0)


# ---------------------------------------------------------------------------
# IMA ADPCM decoder (BLE packets)
# ---------------------------------------------------------------------------
_STEP = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767]
_INDEX_ADJ = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]


def adpcm_decode(data: bytes, predictor: int, index: int) -> np.ndarray:
    out = np.empty(len(data) * 2, dtype=np.int16)
    i = 0
    for byte in data:
        for code in (byte & 0x0F, byte >> 4):
            step = _STEP[index]
            delta = step >> 3
            if code & 4:
                delta += step
            if code & 2:
                delta += step >> 1
            if code & 1:
                delta += step >> 2
            predictor += -delta if code & 8 else delta
            predictor = max(-32768, min(32767, predictor))
            index = max(0, min(88, index + _INDEX_ADJ[code]))
            out[i] = predictor
            i += 1
    return out


# ---------------------------------------------------------------------------
# Common receive side: trigger -> key, audio -> jitter buffer
# ---------------------------------------------------------------------------
class Receiver:
    def __init__(self, key: Key, jb: JitterBuffer, stats: bool, record: str | None = None):
        self.wav = None
        if record:
            self.wav = wave.open(record, "wb")
            self.wav.setnchannels(1)
            self.wav.setsampwidth(2)
            self.wav.setframerate(SAMPLE_RATE)
            print(f"recording received audio to {record}", flush=True)
        self.key = key
        self.jb = jb
        self.stats = stats
        self.last_rx = 0.0
        self._t, self._pkts, self._audio, self._peak = time.monotonic(), 0, 0, 0.0
        self.cb_status: list[str] = []

    def on_packet(self, trigger: bool, pcm: np.ndarray | None) -> None:
        self.last_rx = time.monotonic()
        self.key.set(trigger)
        self._pkts += 1
        if pcm is not None and len(pcm):
            if self.wav:
                self.wav.writeframes(pcm.astype("<i2").tobytes())
            f = pcm.astype(np.float32) / 32768.0
            self.jb.push(f)
            self._audio += 1
            self._peak = max(self._peak, float(np.abs(f).max()))
        if self.stats and self.last_rx - self._t >= 2.0:
            db = 20 * np.log10(max(self._peak, 1e-6))
            print(f"pkts {self._pkts / (self.last_rx - self._t):.0f}/s  audio {self._audio}  "
                  f"peak {db:.0f} dBFS  buf {self.jb.backlog_ms}ms  cb {self.cb_status[-3:]}", flush=True)
            self.cb_status.clear()
            self._t, self._pkts, self._audio, self._peak = self.last_rx, 0, 0, 0.0

    def watchdog(self, stop: threading.Event) -> None:
        """Release the key if the device goes silent while holding it."""
        while not stop.wait(0.1):
            if self.key.down and time.monotonic() - self.last_rx > KEY_TIMEOUT_S:
                self.key.set(False)


# ---------------------------------------------------------------------------
# Transports
# ---------------------------------------------------------------------------
def run_udp(rx: Receiver, stop: threading.Event) -> None:
    threading.Thread(target=beacon_loop, args=(stop,), daemon=True).start()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", LISTEN_PORT))
    sock.settimeout(0.2)
    peer = None
    print("waiting for StopWatch (Wi-Fi)...", flush=True)
    while True:
        try:
            data, addr = sock.recvfrom(2048)
        except socket.timeout:
            continue
        if len(data) < HEADER.size:
            continue
        magic, _seq, flags = HEADER.unpack_from(data)
        if magic != b"SWM1":
            continue
        if addr[0] != peer:
            peer = addr[0]
            print(f"StopWatch connected: {peer}", flush=True)
        payload = data[HEADER.size:]
        rx.on_packet(bool(flags & 1), np.frombuffer(payload, dtype="<i2") if payload else None)


BLE_SERVICE = "7a1e0001-5c3b-4c8e-9a51-2f6d8e3b9c10"
BLE_CHAR = "7a1e0002-5c3b-4c8e-9a51-2f6d8e3b9c10"


def run_ble(rx: Receiver, stop: threading.Event) -> None:
    import asyncio
    from bleak import BleakClient, BleakScanner

    def on_notify(_char, data: bytearray) -> None:
        if len(data) < 2:
            return
        trigger = bool(data[0] & 1)
        pcm = None
        if len(data) > 5:
            predictor = struct.unpack_from("<h", data, 2)[0]
            pcm = adpcm_decode(bytes(data[5:]), predictor, data[4])
        rx.on_packet(trigger, pcm)

    async def loop() -> None:
        while True:
            print("scanning for StopWatch (BLE)...", flush=True)
            dev = await BleakScanner.find_device_by_filter(
                lambda d, ad: BLE_SERVICE in [u.lower() for u in ad.service_uuids], timeout=10.0)
            if dev is None:
                continue
            gone = asyncio.Event()
            try:
                async with BleakClient(dev, disconnected_callback=lambda _c: gone.set()) as client:
                    print(f"StopWatch connected: {dev.name} (mtu {client.mtu_size})", flush=True)
                    await client.start_notify(BLE_CHAR, on_notify)
                    await gone.wait()
            except Exception as e:  # noqa: BLE001 - keep retrying on any BLE error
                print("BLE error:", e, flush=True)
            rx.key.set(False)
            print("StopWatch disconnected", flush=True)
            await asyncio.sleep(1.0)

    asyncio.run(loop())


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--transport", choices=["udp", "ble"], default="udp", help="udp = Wi-Fi firmware, ble = BLE firmware")
    ap.add_argument("--device", default="BlackHole", help="output device name (substring)")
    ap.add_argument("--key", default="f3", choices=sorted(KEYCODES), help="key held while triggered")
    ap.add_argument("--stats", action="store_true", help="print packet rate / level every 2 s")
    ap.add_argument("--record", metavar="WAV", help="also save the received audio to this WAV file")
    args = ap.parse_args()

    dev = find_output_device(args.device)
    dev_info = sd.query_devices(dev)
    channels = min(2, dev_info["max_output_channels"])
    print(f"output: {dev_info['name']} ({channels}ch)  key: {args.key.upper()}  via: {args.transport}", flush=True)

    key = Key(KEYCODES[args.key])
    jb = JitterBuffer()
    rx = Receiver(key, jb, args.stats, args.record)

    def callback(outdata, frames, _time, status):
        if status:
            rx.cb_status.append(str(status))
        mono = jb.pull(frames)
        outdata[:] = np.repeat(mono[:, None], channels, axis=1)

    stop = threading.Event()
    threading.Thread(target=rx.watchdog, args=(stop,), daemon=True).start()
    try:
        # CoreAudio resamples 16 kHz to the device rate
        with sd.OutputStream(device=dev, samplerate=SAMPLE_RATE, channels=channels,
                             dtype="float32", blocksize=CHUNK, callback=callback):
            (run_ble if args.transport == "ble" else run_udp)(rx, stop)
    except KeyboardInterrupt:
        pass
    finally:
        key.set(False)
        stop.set()
        if rx.wav:
            rx.wav.close()


if __name__ == "__main__":
    main()
