//! Tiny persistent settings (key=value lines).
//!   macOS:   ~/Library/Application Support/StopWatch Mic/config.txt
//!   Windows: %APPDATA%\StopWatch Mic\config.txt

use std::path::PathBuf;

fn path() -> Option<PathBuf> {
    #[cfg(target_os = "macos")]
    let base = std::env::var_os("HOME").map(|h| PathBuf::from(h).join("Library/Application Support"));
    #[cfg(target_os = "windows")]
    let base = std::env::var_os("APPDATA").map(PathBuf::from);
    #[cfg(not(any(target_os = "macos", target_os = "windows")))]
    let base = std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".config"));
    base.map(|b| b.join("StopWatch Mic").join("config.txt"))
}

pub fn get(name: &str) -> Option<String> {
    let text = std::fs::read_to_string(path()?).ok()?;
    text.lines().find_map(|l| {
        let (k, v) = l.split_once('=')?;
        (k.trim() == name).then(|| v.trim().to_string())
    })
}

pub fn set(name: &str, value: &str) {
    let Some(p) = path() else { return };
    let mut lines: Vec<String> = std::fs::read_to_string(&p)
        .unwrap_or_default()
        .lines()
        .filter(|l| l.split_once('=').map(|(k, _)| k.trim() != name).unwrap_or(true))
        .map(String::from)
        .collect();
    lines.push(format!("{name}={value}"));
    if let Some(dir) = p.parent() {
        let _ = std::fs::create_dir_all(dir);
    }
    let _ = std::fs::write(&p, lines.join("\n") + "\n");
}
