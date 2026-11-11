//! Output stream into the virtual audio device (BlackHole / VB-CABLE).

use std::sync::Arc;

use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
use cpal::{FromSample, SampleFormat, SizedSample, Stream};

use crate::Shared;

/// Opens the first output device whose name contains `needle`
/// (case-insensitive) at its default config and starts playback.
pub fn start(needle: &str, shared: Arc<Shared>) -> Result<(Stream, String), String> {
    let host = cpal::default_host();
    let needle_lc = needle.to_lowercase();
    let dev = host
        .output_devices()
        .map_err(|e| e.to_string())?
        .find(|d| d.name().map(|n| n.to_lowercase().contains(&needle_lc)).unwrap_or(false))
        .ok_or_else(|| format!("output device \"{needle}\" not found"))?;
    let name = dev.name().unwrap_or_default();
    let cfg = dev.default_output_config().map_err(|e| e.to_string())?;
    let stream = match cfg.sample_format() {
        SampleFormat::F32 => build::<f32>(&dev, &cfg.config(), shared),
        SampleFormat::I16 => build::<i16>(&dev, &cfg.config(), shared),
        SampleFormat::I32 => build::<i32>(&dev, &cfg.config(), shared),
        f => return Err(format!("unsupported sample format {f:?}")),
    }?;
    stream.play().map_err(|e| e.to_string())?;
    Ok((stream, name))
}

fn build<T>(dev: &cpal::Device, cfg: &cpal::StreamConfig, shared: Arc<Shared>) -> Result<Stream, String>
where
    T: SizedSample + FromSample<f32>,
{
    let rate = cfg.sample_rate.0 as f64;
    let channels = cfg.channels as usize;
    dev.build_output_stream(
        cfg,
        move |out: &mut [T], _| {
            let mut jb = shared.jb.lock().unwrap();
            for frame in out.chunks_mut(channels) {
                let v = T::from_sample(jb.next(rate));
                frame.fill(v);
            }
        },
        |e| eprintln!("audio: {e}"),
        None,
    )
    .map_err(|e| e.to_string())
}
