//! swmic-bridge: tray app for the BLE StopWatch mic (macOS / Windows).
//!
//! Connects to the StopWatch over BLE, plays its audio into a virtual audio
//! device (BlackHole on macOS, VB-CABLE on Windows) and holds a function key
//! while the StopWatch button is held.
//!
//! Usage: swmic-bridge [--key f3] [--device <output device name>]

#![cfg_attr(windows, windows_subsystem = "windows")]

mod adpcm;
mod audio;
mod ble;
mod jitter;
mod key;

use std::sync::atomic::{AtomicBool, AtomicU8, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use tao::event::{Event, StartCause};
use tao::event_loop::{ControlFlow, EventLoopBuilder};
use tray_icon::menu::{Menu, MenuEvent, MenuItem, PredefinedMenuItem};
use tray_icon::{Icon, TrayIconBuilder};

use jitter::JitterBuffer;
use key::FKey;

#[cfg(target_os = "windows")]
const DEFAULT_DEVICE: &str = "CABLE Input";
#[cfg(not(target_os = "windows"))]
const DEFAULT_DEVICE: &str = "BlackHole";

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum State {
    Searching = 0,
    Connected = 1,
    OnAir = 2,
    Paused = 3, // disconnected on purpose from the menu
}

pub struct Shared {
    pub jb: Mutex<JitterBuffer>,
    pub key: FKey,
    pub key_down: AtomicBool,
    /// false = stay disconnected (menu "Disconnect"), so another PC can connect
    pub enabled: AtomicBool,
    state: AtomicU8,
    message: Mutex<Option<String>>,
}

impl Shared {
    pub fn state(&self) -> State {
        match self.state.load(Ordering::Relaxed) {
            1 => State::Connected,
            2 => State::OnAir,
            3 => State::Paused,
            _ => State::Searching,
        }
    }
    pub fn set_state(&self, s: State) {
        self.state.store(s as u8, Ordering::Relaxed);
        if s != State::Searching {
            *self.message.lock().unwrap() = None;
        }
    }
    pub fn set_message(&self, m: String) {
        *self.message.lock().unwrap() = Some(m);
    }
}

fn arg(name: &str) -> Option<String> {
    let mut it = std::env::args();
    while let Some(a) = it.next() {
        if a == name {
            return it.next();
        }
        if let Some(v) = a.strip_prefix(&format!("{name}=")) {
            return Some(v.to_string());
        }
    }
    None
}

/// Round status dot for the tray: grey = searching, green = connected,
/// red = on air, hollow ring = disconnected on purpose.
fn icon(state: State) -> Icon {
    const N: u32 = 32;
    let (r, g, b) = match state {
        State::Searching | State::Paused => (0x8a, 0x8a, 0x96),
        State::Connected => (0x2e, 0xc2, 0x7e),
        State::OnAir => (0xff, 0x4d, 0x6a),
    };
    let mut rgba = Vec::with_capacity((N * N * 4) as usize);
    let c = (N as f32 - 1.0) / 2.0;
    for y in 0..N {
        for x in 0..N {
            let d = ((x as f32 - c).powi(2) + (y as f32 - c).powi(2)).sqrt();
            let mut a = (11.5 - d).clamp(0.0, 1.0); // anti-aliased disc, r = 11.5 px
            if state == State::Paused {
                a = a.min((d - 8.5).clamp(0.0, 1.0)); // ring, 3 px wide
            }
            rgba.extend_from_slice(&[r, g, b, (a * 255.0) as u8]);
        }
    }
    Icon::from_rgba(rgba, N, N).expect("icon")
}

fn status_text(state: State) -> &'static str {
    match state {
        State::Searching => "Searching for StopWatch…",
        State::Connected => "Connected",
        State::OnAir => "On air",
        State::Paused => "Disconnected",
    }
}

fn main() {
    let key = arg("--key").and_then(|k| FKey::parse(&k)).unwrap_or(FKey(3));
    let device = arg("--device").unwrap_or_else(|| DEFAULT_DEVICE.to_string());

    let shared = Arc::new(Shared {
        jb: Mutex::new(JitterBuffer::new()),
        key,
        key_down: AtomicBool::new(false),
        enabled: AtomicBool::new(true),
        state: AtomicU8::new(State::Searching as u8),
        message: Mutex::new(None),
    });

    // Audio out. Keep the stream alive for the whole run.
    let (stream, output_line) = match audio::start(&device, shared.clone()) {
        Ok((s, name)) => (Some(s), format!("Output: {name}  ·  Key: F{}", key.0)),
        Err(e) => (None, format!("Audio: {e}")),
    };

    {
        let shared = shared.clone();
        std::thread::spawn(move || ble::run(shared));
    }

    #[allow(unused_mut)]
    let mut event_loop = EventLoopBuilder::new().build();
    #[cfg(target_os = "macos")]
    {
        use tao::platform::macos::{ActivationPolicy, EventLoopExtMacOS};
        event_loop.set_activation_policy(ActivationPolicy::Accessory); // no Dock icon
    }

    let status_item = MenuItem::new(status_text(State::Searching), false, None);
    let detail_item = MenuItem::new(&output_line, false, None);
    let toggle_item = MenuItem::new("Disconnect", true, None);
    let quit_item = MenuItem::new("Quit", true, None);
    let menu = Menu::new();
    menu.append_items(&[
        &status_item,
        &detail_item,
        &PredefinedMenuItem::separator(),
        &toggle_item,
        &PredefinedMenuItem::separator(),
        &quit_item,
    ])
    .expect("menu");

    let mut menu = Some(menu);
    let mut tray = None;
    let mut shown: Option<(State, Option<String>)> = None;
    let menu_rx = MenuEvent::receiver();

    event_loop.run(move |event, _, control_flow| {
        let _keep_audio_alive = &stream;
        *control_flow = ControlFlow::WaitUntil(Instant::now() + Duration::from_millis(100));

        if let Event::NewEvents(StartCause::Init) = event {
            tray = Some(
                TrayIconBuilder::new()
                    .with_menu(Box::new(menu.take().unwrap()))
                    .with_tooltip("StopWatch Mic")
                    .with_icon(icon(State::Searching))
                    .build()
                    .expect("tray icon"),
            );
        }

        if let Ok(ev) = menu_rx.try_recv() {
            if &ev.id == toggle_item.id() {
                let enable = !shared.enabled.load(Ordering::Relaxed);
                shared.enabled.store(enable, Ordering::Relaxed);
                toggle_item.set_text(if enable { "Disconnect" } else { "Connect" });
            }
            if &ev.id == quit_item.id() {
                if shared.key_down.swap(false, Ordering::Relaxed) {
                    key::send(shared.key, false);
                }
                tray.take();
                *control_flow = ControlFlow::Exit;
                return;
            }
        }

        let now = (shared.state(), shared.message.lock().unwrap().clone());
        if shown.as_ref() != Some(&now) {
            if let Some(t) = &tray {
                let _ = t.set_icon(Some(icon(now.0)));
                let text = now.1.clone().unwrap_or_else(|| status_text(now.0).to_string());
                status_item.set_text(&text);
                let _ = t.set_tooltip(Some(format!("StopWatch Mic — {text}")));
                shown = Some(now);
            }
        }
    });
}
