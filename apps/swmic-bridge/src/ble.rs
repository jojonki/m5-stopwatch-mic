//! BLE link to the StopWatch (firmware/ble): scan, connect, subscribe, and
//! turn packets into audio + trigger key events. Reconnects forever.
//!
//! Packet (one notify characteristic):
//!   audio   : u8 flags | u8 seq | i16le predictor | u8 step_index | IMA ADPCM
//!   trigger : u8 flags | u8 seq
//!   flags bit0 = trigger held

use std::error::Error;
use std::sync::atomic::Ordering;
use std::sync::Arc;
use std::time::{Duration, Instant};

use btleplug::api::{Central, Manager as _, Peripheral as _, ScanFilter};
use btleplug::platform::{Adapter, Manager, Peripheral};
use futures::StreamExt;
use uuid::Uuid;

use crate::{adpcm, key, Shared, State};

const SERVICE: Uuid = Uuid::from_u128(0x7a1e0001_5c3b_4c8e_9a51_2f6d8e3b9c10);
const PACKETS: Uuid = Uuid::from_u128(0x7a1e0002_5c3b_4c8e_9a51_2f6d8e3b9c10);
const KEY_TIMEOUT: Duration = Duration::from_millis(500);

type Res<T> = Result<T, Box<dyn Error>>;

pub fn run(shared: Arc<Shared>) {
    let rt = tokio::runtime::Builder::new_current_thread().enable_all().build().expect("tokio");
    rt.block_on(async move {
        let adapter = loop {
            match first_adapter().await {
                Ok(a) => break a,
                Err(e) => {
                    shared.set_message(format!("Bluetooth unavailable: {e}"));
                    tokio::time::sleep(Duration::from_secs(3)).await;
                }
            }
        };
        loop {
            shared.set_state(State::Searching);
            if let Err(e) = session(&adapter, &shared).await {
                shared.set_message(format!("BLE: {e}"));
            }
            set_key(&shared, false);
            tokio::time::sleep(Duration::from_secs(1)).await;
        }
    });
}

async fn first_adapter() -> Res<Adapter> {
    let manager = Manager::new().await?;
    Ok(manager.adapters().await?.into_iter().next().ok_or("no Bluetooth adapter")?)
}

async fn find_device(adapter: &Adapter) -> Res<Peripheral> {
    adapter.start_scan(ScanFilter { services: vec![SERVICE] }).await?;
    loop {
        for p in adapter.peripherals().await? {
            if let Ok(Some(props)) = p.properties().await {
                if props.services.contains(&SERVICE) {
                    adapter.stop_scan().await.ok();
                    return Ok(p);
                }
            }
        }
        tokio::time::sleep(Duration::from_millis(500)).await;
    }
}

async fn session(adapter: &Adapter, shared: &Shared) -> Res<()> {
    let dev = find_device(adapter).await?;
    dev.connect().await?;
    dev.discover_services().await?;
    let ch = dev
        .characteristics()
        .into_iter()
        .find(|c| c.uuid == PACKETS)
        .ok_or("StopWatch characteristic not found")?;
    let mut notes = dev.notifications().await?;
    dev.subscribe(&ch).await?;
    shared.set_state(State::Connected);

    let mut last_rx = Instant::now();
    let mut last_check = Instant::now();
    let mut tick = tokio::time::interval(Duration::from_millis(100));
    let mut pcm = Vec::with_capacity(512);

    loop {
        tokio::select! {
            n = notes.next() => {
                let Some(n) = n else { return Ok(()) };  // stream ended: disconnected
                if n.uuid != PACKETS || n.value.len() < 2 { continue; }
                last_rx = Instant::now();
                let v = &n.value;
                set_key(shared, v[0] & 1 != 0);
                if v.len() > 5 {
                    let predictor = i16::from_le_bytes([v[2], v[3]]) as i32;
                    pcm.clear();
                    adpcm::decode(&v[5..], predictor, v[4] as i32, &mut pcm);
                    shared.jb.lock().unwrap().push(&pcm);
                }
            }
            _ = tick.tick() => {
                // never leave the key stuck if the device goes quiet mid-press
                if shared.key_down.load(Ordering::Relaxed) && last_rx.elapsed() > KEY_TIMEOUT {
                    set_key(shared, false);
                }
                if last_check.elapsed() > Duration::from_secs(1) {
                    last_check = Instant::now();
                    if !dev.is_connected().await.unwrap_or(false) {
                        return Ok(());
                    }
                }
            }
        }
    }
}

fn set_key(shared: &Shared, down: bool) {
    if shared.key_down.swap(down, Ordering::Relaxed) != down {
        key::send(shared.key, down);
        if shared.state() != State::Searching {
            shared.set_state(if down { State::OnAir } else { State::Connected });
        }
    }
}
