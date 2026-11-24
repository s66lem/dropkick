//! 1 Hz background monitor thread: system telemetry (CPU, memory, temperature) and
//! workshop folder hot-reload watcher.

use crate::state::{Command, SharedState, TelemetrySnapshot};
use std::collections::HashMap;
use std::fs;
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, SystemTime};
use sysinfo::{Components, System};

pub fn spawn_monitor(state: Arc<SharedState>, workshop_dir: PathBuf, stop: Arc<AtomicBool>) -> std::thread::JoinHandle<()> {
    std::thread::spawn(move || {
        let mut sys = System::new();
        let mut components = Components::new_with_refreshed_list();
        let mut workshop_mtimes: HashMap<PathBuf, SystemTime> = scan_workshop(&workshop_dir);

        while !stop.load(Ordering::Relaxed) {
            sys.refresh_cpu_usage();
            sys.refresh_memory();
            components.refresh(true);

            let cpu = sys.global_cpu_usage().clamp(0.0, 100.0).round() as u32;
            let mem_used = sys.used_memory() / (1024 * 1024);
            let mem_total = sys.total_memory() / (1024 * 1024);

            let mut best_temp: Option<f32> = None;
            for c in components.list() {
                if let Some(t) = c.temperature() {
                    if t > 0.0 && t < 150.0 {
                        best_temp = Some(best_temp.map_or(t, |cur| cur.max(t)));
                    }
                }
            }
            // Fallback Linux thermal zone read (Raspberry Pi SoC)
            #[cfg(target_os = "linux")]
            if best_temp.is_none() {
                if let Ok(raw) = fs::read_to_string("/sys/class/thermal/thermal_zone0/temp") {
                    if let Ok(milli) = raw.trim().parse::<f32>() {
                        if milli > 0.0 {
                            best_temp = Some(milli / 1000.0);
                        }
                    }
                }
            }

            {
                let mut tel = state.telemetry.lock().unwrap();
                *tel = TelemetrySnapshot {
                    cpu,
                    mem_used,
                    mem_total,
                    temp: best_temp.map(|t| t.round() as u32).unwrap_or(0),
                    has_temp: best_temp.is_some(),
                };
            }

            // Poll workshop directory for external edits to .milk files (skip _scratch.milk
            // which is already loaded directly by POST /api/workshop/apply).
            let current = scan_workshop(&workshop_dir);
            for (path, mtime) in &current {
                let is_scratch = path
                    .file_name()
                    .and_then(|s| s.to_str())
                    .map(|s| s == "_scratch.milk")
                    .unwrap_or(false);
                if is_scratch {
                    continue;
                }
                if workshop_mtimes.get(path) != Some(mtime) {
                    state.push_cmd(Command::LoadWorkshopFile(path.clone()));
                }
            }
            workshop_mtimes = current;

            for _ in 0..10 {
                if stop.load(Ordering::Relaxed) {
                    break;
                }
                std::thread::sleep(Duration::from_millis(100));
            }
        }
    })
}

fn scan_workshop(dir: &PathBuf) -> HashMap<PathBuf, SystemTime> {
    let mut map = HashMap::new();
    if let Ok(rd) = fs::read_dir(dir) {
        for entry in rd.flatten() {
            let p = entry.path();
            let ext = p.extension().and_then(|s| s.to_str()).unwrap_or("");
            if ext.eq_ignore_ascii_case("milk") || ext.eq_ignore_ascii_case("milk2") {
                if let Ok(meta) = entry.metadata() {
                    if let Ok(mtime) = meta.modified() {
                        map.insert(p, mtime);
                    }
                }
            }
        }
    }
    map
}
