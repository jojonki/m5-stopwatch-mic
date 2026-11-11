//! Update check / self-update from GitHub Releases.
//!
//! Uses the system `curl` (bundled with macOS and Windows 10+) so the app does
//! not need its own HTTPS stack. Release assets are expected to be named:
//!   macOS:   StopWatch-Mic-macOS.zip        (contains "StopWatch Mic.app")
//!   Windows: swmic-bridge-windows-x64.zip   (contains "swmic-bridge.exe")

use std::path::Path;
use std::process::Command;

const LATEST_URL: &str = "https://api.github.com/repos/jojonki/m5-stopwatch-mic/releases/latest";
pub const CURRENT: &str = env!("CARGO_PKG_VERSION");

#[cfg(target_os = "macos")]
const ASSET: &str = "StopWatch-Mic-macOS.zip";
#[cfg(target_os = "windows")]
const ASSET: &str = "swmic-bridge-windows-x64.zip";
#[cfg(not(any(target_os = "macos", target_os = "windows")))]
const ASSET: &str = "";

#[derive(Clone, Debug, PartialEq)]
pub struct Release {
    pub version: String, // without the leading "v"
    pub url: String,     // asset download URL
}

fn command(program: &str) -> Command {
    #[allow(unused_mut)]
    let mut c = Command::new(program);
    #[cfg(target_os = "windows")]
    {
        use std::os::windows::process::CommandExt;
        c.creation_flags(0x0800_0000); // CREATE_NO_WINDOW: no console flash
    }
    c
}

fn run(c: &mut Command) -> Result<Vec<u8>, String> {
    let out = c.output().map_err(|e| e.to_string())?;
    if !out.status.success() {
        return Err(String::from_utf8_lossy(&out.stderr).trim().to_string());
    }
    Ok(out.stdout)
}

fn version_tuple(v: &str) -> (u32, u32, u32) {
    let mut it = v.trim_start_matches('v').split('.').map(|p| {
        p.chars().take_while(|c| c.is_ascii_digit()).collect::<String>().parse().unwrap_or(0)
    });
    (it.next().unwrap_or(0), it.next().unwrap_or(0), it.next().unwrap_or(0))
}

/// Latest release if it is newer than this build.
pub fn check() -> Result<Option<Release>, String> {
    let body = run(command("curl").args([
        "-fsSL",
        "--max-time",
        "15",
        "-H",
        "Accept: application/vnd.github+json",
        "-H",
        "User-Agent: swmic-bridge",
        LATEST_URL,
    ]))?;
    let json: serde_json::Value = serde_json::from_slice(&body).map_err(|e| e.to_string())?;
    let tag = json["tag_name"].as_str().ok_or("no tag_name in release")?;
    let version = tag.trim_start_matches('v').to_string();
    if version_tuple(&version) <= version_tuple(CURRENT) {
        return Ok(None);
    }
    let url = json["assets"]
        .as_array()
        .and_then(|a| a.iter().find(|x| x["name"] == ASSET))
        .and_then(|x| x["browser_download_url"].as_str())
        .ok_or_else(|| format!("release {tag} has no {ASSET}"))?;
    Ok(Some(Release { version, url: url.to_string() }))
}

fn download(url: &str, to: &Path) -> Result<(), String> {
    run(command("curl").args(["-fsSL", "--max-time", "300", "-o"]).arg(to).arg(url)).map(|_| ())
}

/// Downloads `rel`, replaces this app and starts the new one. On success the
/// caller should exit right away.
#[cfg(target_os = "macos")]
pub fn install(rel: &Release) -> Result<(), String> {
    let exe = std::env::current_exe().map_err(|e| e.to_string())?;
    // .../StopWatch Mic.app/Contents/MacOS/swmic-bridge
    let app = exe.ancestors().nth(3).ok_or("not running from an .app bundle")?.to_path_buf();
    if app.extension().and_then(|e| e.to_str()) != Some("app") {
        return Err("not running from an .app bundle".into());
    }
    let parent = app.parent().ok_or("bad app path")?;
    let work = parent.join(".swmic-update");
    let _ = std::fs::remove_dir_all(&work);
    std::fs::create_dir_all(&work).map_err(|e| format!("{}: {e}", work.display()))?;

    let zip = work.join("update.zip");
    download(&rel.url, &zip)?;
    run(command("ditto").args(["-x", "-k"]).arg(&zip).arg(work.join("x")))?;
    let new_app = work.join("x").join("StopWatch Mic.app");
    if !new_app.exists() {
        return Err("update archive has no StopWatch Mic.app".into());
    }

    // Swap bundles via renames in the same directory, so a failure leaves the
    // old app in place.
    let old = work.join("old.app");
    std::fs::rename(&app, &old).map_err(|e| format!("replace app: {e}"))?;
    if let Err(e) = std::fs::rename(&new_app, &app) {
        let _ = std::fs::rename(&old, &app);
        return Err(format!("replace app: {e}"));
    }
    let _ = std::fs::remove_dir_all(&work);

    command("open").arg("-n").arg(&app).spawn().map_err(|e| e.to_string())?;
    Ok(())
}

#[cfg(target_os = "windows")]
pub fn install(rel: &Release) -> Result<(), String> {
    let exe = std::env::current_exe().map_err(|e| e.to_string())?;
    let dir = exe.parent().ok_or("bad exe path")?;
    let work = dir.join("swmic-update");
    let _ = std::fs::remove_dir_all(&work);
    std::fs::create_dir_all(&work).map_err(|e| format!("{}: {e}", work.display()))?;

    let zip = work.join("update.zip");
    download(&rel.url, &zip)?;
    run(command("tar").arg("-xf").arg(&zip).arg("-C").arg(&work))?;
    let new_exe = work.join("swmic-bridge.exe");
    if !new_exe.exists() {
        return Err("update archive has no swmic-bridge.exe".into());
    }

    // A running exe cannot be overwritten but can be renamed.
    let old = old_exe_path(&exe);
    let _ = std::fs::remove_file(&old);
    std::fs::rename(&exe, &old).map_err(|e| format!("replace exe: {e}"))?;
    if let Err(e) = std::fs::rename(&new_exe, &exe) {
        let _ = std::fs::rename(&old, &exe);
        return Err(format!("replace exe: {e}"));
    }
    let _ = std::fs::remove_dir_all(&work);

    command(exe.to_str().ok_or("bad exe path")?).spawn().map_err(|e| e.to_string())?;
    Ok(())
}

#[cfg(not(any(target_os = "macos", target_os = "windows")))]
pub fn install(_rel: &Release) -> Result<(), String> {
    Err("self-update is not supported on this platform".into())
}

#[cfg(target_os = "windows")]
fn old_exe_path(exe: &Path) -> std::path::PathBuf {
    exe.with_extension("exe.old")
}

/// Removes leftovers of a previous update (Windows keeps the old exe until the
/// next start).
pub fn cleanup() {
    #[cfg(target_os = "windows")]
    if let Ok(exe) = std::env::current_exe() {
        let _ = std::fs::remove_file(old_exe_path(&exe));
    }
}

#[cfg(test)]
mod tests {
    use super::version_tuple;

    #[test]
    fn compares_versions() {
        assert!(version_tuple("v0.2.0") > version_tuple("0.1.9"));
        assert!(version_tuple("0.10.0") > version_tuple("0.9.3"));
        assert_eq!(version_tuple("v1.2.3-beta"), (1, 2, 3));
    }
}
