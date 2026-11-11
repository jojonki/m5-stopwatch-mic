//! swmic-bridge: tray app for the BLE StopWatch mic (macOS / Windows).
//!
//! Connects to the StopWatch over BLE, plays its audio into a virtual audio
//! device (BlackHole on macOS, VB-CABLE on Windows) and holds a function key
//! while the StopWatch button is held.
//!
//! The trigger key is chosen from the tray menu (saved), or with --key.
//! Updates are checked against GitHub Releases at startup and daily.
//!
//! Usage: swmic-bridge [--key f3] [--device <output device name>]

#![cfg_attr(windows, windows_subsystem = "windows")]

mod adpcm;
mod audio;
mod ble;
mod config;
mod icons;
mod jitter;
mod key;
mod log;
mod update;

use std::sync::atomic::{AtomicBool, AtomicU8, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use tao::event::{Event, StartCause};
use tao::event_loop::{ControlFlow, EventLoopBuilder};
use tray_icon::menu::{CheckMenuItem, Menu, MenuEvent, MenuItem, PredefinedMenuItem, Submenu};
use tray_icon::TrayIconBuilder;

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

pub enum UpdateState {
    Idle,
    Checking,
    UpToDate,
    Available(update::Release),
    Installing,
    Failed(String),
}

pub struct Shared {
    pub jb: Mutex<JitterBuffer>,
    key: AtomicU8, // function key number, changeable from the menu
    pub key_down: AtomicBool,
    /// false = stay disconnected (menu "Disconnect"), so another PC can connect
    pub enabled: AtomicBool,
    state: AtomicU8,
    message: Mutex<Option<String>>,
    update: Mutex<UpdateState>,
    exit_requested: AtomicBool,
}

impl Shared {
    pub fn key(&self) -> FKey {
        FKey(self.key.load(Ordering::Relaxed))
    }
    /// Switch the trigger key; a held key is moved over to the new one.
    pub fn set_key(&self, k: FKey) {
        let old = FKey(self.key.swap(k.0, Ordering::Relaxed));
        if old.0 != k.0 && self.key_down.load(Ordering::Relaxed) {
            key::send(old, false);
            key::send(k, true);
        }
    }
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

fn status_text(state: State) -> &'static str {
    match state {
        State::Searching => "Searching for StopWatch…",
        State::Connected => "Connected",
        State::OnAir => "On air",
        State::Paused => "Disconnected",
    }
}

fn update_label(u: &UpdateState) -> String {
    match u {
        UpdateState::Idle => format!("Check for updates (v{})", update::CURRENT),
        UpdateState::Checking => "Checking for updates…".into(),
        UpdateState::UpToDate => format!("Up to date (v{})", update::CURRENT),
        UpdateState::Available(r) => format!("Update to v{} and restart", r.version),
        UpdateState::Installing => "Updating…".into(),
        UpdateState::Failed(e) => format!("Update failed: {e}"),
    }
}

fn spawn_check(shared: &Arc<Shared>) {
    let shared = shared.clone();
    std::thread::spawn(move || {
        *shared.update.lock().unwrap() = UpdateState::Checking;
        let st = match update::check() {
            Ok(Some(r)) => UpdateState::Available(r),
            Ok(None) => UpdateState::UpToDate,
            Err(e) => UpdateState::Failed(e.chars().take(60).collect()),
        };
        *shared.update.lock().unwrap() = st;
    });
}

fn spawn_install(shared: &Arc<Shared>, rel: update::Release) {
    let shared = shared.clone();
    std::thread::spawn(move || {
        *shared.update.lock().unwrap() = UpdateState::Installing;
        match update::install(&rel) {
            Ok(()) => shared.exit_requested.store(true, Ordering::Relaxed),
            Err(e) => *shared.update.lock().unwrap() = UpdateState::Failed(e.chars().take(60).collect()),
        }
    });
}

fn main() {
    update::cleanup();
    let key = arg("--key")
        .or_else(|| config::get("key"))
        .and_then(|k| FKey::parse(&k))
        .unwrap_or(FKey(3));
    let device = arg("--device").unwrap_or_else(|| DEFAULT_DEVICE.to_string());

    let shared = Arc::new(Shared {
        jb: Mutex::new(JitterBuffer::new()),
        key: AtomicU8::new(key.0),
        key_down: AtomicBool::new(false),
        enabled: AtomicBool::new(true),
        state: AtomicU8::new(State::Searching as u8),
        message: Mutex::new(None),
        update: Mutex::new(UpdateState::Idle),
        exit_requested: AtomicBool::new(false),
    });

    // Audio out. Keep the stream alive for the whole run.
    let (stream, output_line) = match audio::start(&device, shared.clone()) {
        Ok((s, name)) => (Some(s), format!("Output: {name}")),
        Err(e) => (None, format!("Audio: {e}")),
    };

    {
        let shared = shared.clone();
        std::thread::spawn(move || ble::run(shared));
    }
    // update check shortly after start, then daily
    {
        let shared = shared.clone();
        std::thread::spawn(move || loop {
            std::thread::sleep(Duration::from_secs(5));
            if matches!(*shared.update.lock().unwrap(), UpdateState::Idle | UpdateState::UpToDate | UpdateState::Failed(_)) {
                spawn_check(&shared);
            }
            std::thread::sleep(Duration::from_secs(24 * 60 * 60));
        });
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
    let key_menu = Submenu::new(format!("Trigger key: F{}", key.0), true);
    let key_items: Vec<CheckMenuItem> = (1..=20u8)
        .map(|n| CheckMenuItem::new(format!("F{n}"), true, n == key.0, None))
        .collect();
    for item in &key_items {
        key_menu.append(item).expect("menu");
    }
    let toggle_item = MenuItem::new("Disconnect", true, None);
    // macOS: shown only while the Accessibility permission is missing
    let perm_item = MenuItem::new("⚠ Allow Accessibility to send the key…", true, None);
    let update_item = MenuItem::new(update_label(&UpdateState::Idle), true, None);
    let quit_item = MenuItem::new("Quit", true, None);
    let menu = Menu::new();
    menu.append_items(&[
        &status_item,
        &detail_item,
        &PredefinedMenuItem::separator(),
        &perm_item,
        &key_menu,
        &toggle_item,
        &PredefinedMenuItem::separator(),
        &update_item,
        &quit_item,
    ])
    .expect("menu");
    let mut shown_update = String::new();
    // Ask once at startup (macOS shows its dialog), then poll quietly.
    let mut key_ok = key::can_send(true);
    log::line(&format!("start v{} key F{} post-event access: {key_ok}", update::CURRENT, key.0));
    let mut perm_shown = true;
    let mut last_perm_check = Instant::now();

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
                    .with_icon(icons::tray(State::Searching))
                    .with_icon_as_template(icons::IS_TEMPLATE)
                    .build()
                    .expect("tray icon"),
            );
        }

        if let Ok(ev) = menu_rx.try_recv() {
            if let Some(i) = key_items.iter().position(|it| &ev.id == it.id()) {
                let k = FKey(i as u8 + 1);
                shared.set_key(k);
                config::set("key", &format!("f{}", k.0));
                for (j, it) in key_items.iter().enumerate() {
                    it.set_checked(j == i);
                }
                key_menu.set_text(format!("Trigger key: F{}", k.0));
            }
            if &ev.id == perm_item.id() {
                key::open_permission_settings();
            }
            if &ev.id == update_item.id() {
                let available = match &*shared.update.lock().unwrap() {
                    UpdateState::Available(r) => Some(r.clone()),
                    UpdateState::Checking | UpdateState::Installing => return,
                    _ => None,
                };
                match available {
                    Some(r) => spawn_install(&shared, r),
                    None => spawn_check(&shared),
                }
            }
            if &ev.id == toggle_item.id() {
                let enable = !shared.enabled.load(Ordering::Relaxed);
                shared.enabled.store(enable, Ordering::Relaxed);
                toggle_item.set_text(if enable { "Disconnect" } else { "Connect" });
            }
            if &ev.id == quit_item.id() {
                shared.exit_requested.store(true, Ordering::Relaxed);
            }
        }

        if shared.exit_requested.load(Ordering::Relaxed) {
            if shared.key_down.swap(false, Ordering::Relaxed) {
                key::send(shared.key(), false);
            }
            tray.take();
            *control_flow = ControlFlow::Exit;
            return;
        }

        if last_perm_check.elapsed() > Duration::from_secs(2) {
            last_perm_check = Instant::now();
            key_ok = key::can_send(false);
        }
        if perm_shown == key_ok {
            if key_ok {
                perm_item.set_enabled(false);
                perm_item.set_text("Key events: allowed");
            } else {
                perm_item.set_enabled(true);
                perm_item.set_text("⚠ Allow Accessibility to send the key…");
            }
            perm_shown = !key_ok;
        }

        let label = update_label(&shared.update.lock().unwrap());
        if label != shown_update {
            let busy = label == update_label(&UpdateState::Checking) || label == update_label(&UpdateState::Installing);
            update_item.set_text(&label);
            update_item.set_enabled(!busy);
            shown_update = label;
        }

        let now = (shared.state(), shared.message.lock().unwrap().clone());
        if shown.as_ref() != Some(&now) {
            if let Some(t) = &tray {
                let _ = t.set_icon_with_as_template(Some(icons::tray(now.0)), icons::IS_TEMPLATE);
                let text = now.1.clone().unwrap_or_else(|| status_text(now.0).to_string());
                status_item.set_text(&text);
                let _ = t.set_tooltip(Some(format!("StopWatch Mic — {text}")));
                shown = Some(now);
            }
        }
    });
}
