//! Synthetic trigger key (F1-F20) that other apps see as a real key hold.

/// Function key number 1..=20.
#[derive(Clone, Copy)]
pub struct FKey(pub u8);

impl FKey {
    pub fn parse(s: &str) -> Option<Self> {
        let n: u8 = s.trim().to_ascii_lowercase().strip_prefix('f')?.parse().ok()?;
        (1..=20).contains(&n).then_some(FKey(n))
    }
}

#[cfg(target_os = "macos")]
pub fn send(key: FKey, down: bool) {
    use core_graphics::event::{CGEvent, CGEventFlags, CGEventTapLocation};
    use core_graphics::event_source::{CGEventSource, CGEventSourceStateID};

    // macOS virtual key codes for F1..F20
    const CODES: [u16; 20] =
        [122, 120, 99, 118, 96, 97, 98, 100, 101, 109, 103, 111, 105, 107, 113, 106, 64, 79, 80, 90];
    // HID-system source: the key also shows as held in the global key state,
    // which push-to-talk apps poll.
    let Ok(src) = CGEventSource::new(CGEventSourceStateID::HIDSystemState) else { return };
    let Ok(ev) = CGEvent::new_keyboard_event(src, CODES[key.0 as usize - 1], down) else { return };
    // Hardware F-keys carry the Fn flag; strict hotkey matchers may expect it.
    ev.set_flags(CGEventFlags::CGEventFlagSecondaryFn | CGEventFlags::CGEventFlagNonCoalesced);
    ev.post(CGEventTapLocation::HID);
    crate::log::line(&format!("key F{} {} (post-event access: {})", key.0, if down { "down" } else { "up" }, can_send(false)));
}

#[cfg(target_os = "windows")]
pub fn send(key: FKey, down: bool) {
    use windows_sys::Win32::UI::Input::KeyboardAndMouse::{
        MapVirtualKeyW, SendInput, INPUT, INPUT_0, INPUT_KEYBOARD, KEYBDINPUT, KEYEVENTF_KEYUP,
        MAPVK_VK_TO_VSC,
    };
    let vk = 0x70 + (key.0 as u16 - 1); // VK_F1..VK_F20
    let input = INPUT {
        r#type: INPUT_KEYBOARD,
        Anonymous: INPUT_0 {
            ki: KEYBDINPUT {
                wVk: vk,
                wScan: unsafe { MapVirtualKeyW(vk as u32, MAPVK_VK_TO_VSC) } as u16,
                dwFlags: if down { 0 } else { KEYEVENTF_KEYUP },
                time: 0,
                dwExtraInfo: 0,
            },
        },
    };
    unsafe { SendInput(1, &input, std::mem::size_of::<INPUT>() as i32) };
}

#[cfg(not(any(target_os = "macos", target_os = "windows")))]
pub fn send(_key: FKey, _down: bool) {}

/// Whether synthetic key events will reach other apps. On macOS this is the
/// "post events" permission (listed under Accessibility); without it events
/// are silently dropped. With `prompt`, macOS asks for it (and lists the app
/// in System Settings) if it is missing.
#[cfg(target_os = "macos")]
pub fn can_send(prompt: bool) -> bool {
    #[link(name = "CoreGraphics", kind = "framework")]
    extern "C" {
        fn CGPreflightPostEventAccess() -> bool;
        fn CGRequestPostEventAccess() -> bool;
    }
    #[link(name = "ApplicationServices", kind = "framework")]
    extern "C" {
        fn AXIsProcessTrusted() -> bool;
    }
    unsafe {
        // CGPreflightPostEventAccess is cached for the life of the process, so
        // a grant made while running only shows up via AXIsProcessTrusted.
        if AXIsProcessTrusted() || CGPreflightPostEventAccess() {
            return true;
        }
        if prompt {
            return CGRequestPostEventAccess();
        }
        false
    }
}

#[cfg(not(target_os = "macos"))]
pub fn can_send(_prompt: bool) -> bool {
    true
}

/// Opens the system settings page where the permission is granted.
pub fn open_permission_settings() {
    #[cfg(target_os = "macos")]
    let _ = std::process::Command::new("open")
        .arg("x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility")
        .spawn();
}
