//! Jitter buffer with resampling and latency control.
//!
//! Input is 16 kHz mono; output runs at the device rate. Playback starts once
//! PRIME is buffered. BLE delivers packets in bursts and the device sends a
//! 100 ms pre-roll at once, so while more than TARGET is queued playback runs
//! CATCHUP x faster until it is back on target: nothing is dropped and the
//! small speed-up is hard to notice in speech.

use std::collections::VecDeque;

pub const IN_RATE: f64 = 16_000.0;
const PRIME: usize = 16 * 40; // 40 ms
const TARGET: usize = 16 * 60; // 60 ms
const MAX: usize = 16 * 500; // 500 ms: runaway guard
const CATCHUP: f64 = 1.1;

pub struct JitterBuffer {
    buf: VecDeque<f32>,
    pos: f64, // fractional read position into buf
    primed: bool,
}

impl JitterBuffer {
    pub fn new() -> Self {
        Self { buf: VecDeque::with_capacity(MAX + 64), pos: 0.0, primed: false }
    }

    pub fn push(&mut self, samples: &[f32]) {
        self.buf.extend(samples);
        while self.buf.len() > MAX {
            self.buf.pop_front();
        }
    }

    pub fn backlog_ms(&self) -> usize {
        self.buf.len() / 16
    }

    /// Next output sample at `out_rate`.
    pub fn next(&mut self, out_rate: f64) -> f32 {
        if !self.primed {
            if self.buf.len() < PRIME {
                return 0.0;
            }
            self.primed = true;
        }
        if self.buf.len() < 2 {
            self.primed = false; // underrun: re-prime
            self.buf.clear();
            self.pos = 0.0;
            return 0.0;
        }
        let i = self.pos as usize;
        let frac = (self.pos - i as f64) as f32;
        let s = self.buf[i] * (1.0 - frac) + self.buf[i + 1] * frac;

        let speed = if self.buf.len() > TARGET { CATCHUP } else { 1.0 };
        self.pos += IN_RATE / out_rate * speed;
        while self.pos >= 1.0 && self.buf.len() > 1 {
            self.buf.pop_front();
            self.pos -= 1.0;
        }
        s
    }
}
