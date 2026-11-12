//! Output stream into the virtual audio device (BlackHole / VB-CABLE).
//!
//! The stream is supervised: if the device reports an error (on Windows,
//! WASAPI invalidates the stream when the device is reconfigured, the audio
//! service restarts, or the PC wakes from sleep) or the callback stops being
//! called, it is closed and reopened. A device that is missing at startup is
//! retried too.

use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
use cpal::{FromSample, SampleFormat, SizedSample, Stream};

use crate::{log, Shared};

const RETRY: Duration = Duration::from_secs(2);
const STALL: Duration = Duration::from_secs(3);

/// Runs the supervised output stream on its own thread (cpal streams are not
/// Send, so the stream lives and dies on that thread).
pub fn spawn(needle: String, shared: Arc<Shared>) {
    std::thread::spawn(move || run(&needle, &shared));
}

fn run(needle: &str, shared: &Arc<Shared>) {
    let mut last_err = String::new();
    loop {
        let failed = Arc::new(AtomicBool::new(false));
        let ticks = Arc::new(AtomicU64::new(0));
        match open(needle, shared, failed.clone(), ticks.clone()) {
            Ok((stream, name)) => {
                log::line(&format!("audio: opened \"{name}\""));
                shared.set_output(format!("Output: {name}"));
                last_err.clear();
                let mut last = 0;
                let mut moved = Instant::now();
                loop {
                    std::thread::sleep(Duration::from_millis(500));
                    if failed.load(Ordering::Relaxed) {
                        break;
                    }
                    let t = ticks.load(Ordering::Relaxed);
                    if t != last {
                        last = t;
                        moved = Instant::now();
                    } else if moved.elapsed() > STALL {
                        log::line("audio: stream stalled, reopening");
                        break;
                    }
                }
                drop(stream);
                shared.set_output("Output: reconnecting…".into());
            }
            Err(e) => {
                if e != last_err {
                    log::line(&format!("audio: {e}"));
                    shared.set_output(format!("Audio: {e}"));
                    last_err = e;
                }
            }
        }
        std::thread::sleep(RETRY);
    }
}

/// Opens the first output device whose name contains `needle`
/// (case-insensitive) at its default config and starts playback.
fn open(
    needle: &str,
    shared: &Arc<Shared>,
    failed: Arc<AtomicBool>,
    ticks: Arc<AtomicU64>,
) -> Result<(Stream, String), String> {
    let host = cpal::default_host();
    let needle_lc = needle.to_lowercase();
    let dev = host
        .output_devices()
        .map_err(|e| e.to_string())?
        .find(|d| d.name().map(|n| n.to_lowercase().contains(&needle_lc)).unwrap_or(false))
        .ok_or_else(|| format!("output device \"{needle}\" not found"))?;
    let name = dev.name().unwrap_or_default();
    let cfg = dev.default_output_config().map_err(|e| e.to_string())?;
    log::line(&format!(
        "audio: \"{name}\" {} Hz {} ch {:?}",
        cfg.sample_rate().0,
        cfg.channels(),
        cfg.sample_format()
    ));
    let s = shared.clone();
    let stream = match cfg.sample_format() {
        SampleFormat::F32 => build::<f32>(&dev, &cfg.config(), s, failed, ticks),
        SampleFormat::I16 => build::<i16>(&dev, &cfg.config(), s, failed, ticks),
        SampleFormat::I32 => build::<i32>(&dev, &cfg.config(), s, failed, ticks),
        f => return Err(format!("unsupported sample format {f:?}")),
    }?;
    stream.play().map_err(|e| e.to_string())?;
    Ok((stream, name))
}

fn build<T>(
    dev: &cpal::Device,
    cfg: &cpal::StreamConfig,
    shared: Arc<Shared>,
    failed: Arc<AtomicBool>,
    ticks: Arc<AtomicU64>,
) -> Result<Stream, String>
where
    T: SizedSample + FromSample<f32>,
{
    let rate = cfg.sample_rate.0 as f64;
    let channels = cfg.channels as usize;
    dev.build_output_stream(
        cfg,
        move |out: &mut [T], _| {
            ticks.fetch_add(1, Ordering::Relaxed);
            let mut jb = shared.jb.lock().unwrap();
            for frame in out.chunks_mut(channels) {
                let v = T::from_sample(jb.next(rate));
                frame.fill(v);
            }
        },
        move |e| {
            log::line(&format!("audio: stream error: {e}"));
            failed.store(true, Ordering::Relaxed);
        },
        None,
    )
    .map_err(|e| e.to_string())
}
