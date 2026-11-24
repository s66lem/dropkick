//! Shared Dropkick state, playlist navigation, atomic persistence (favorites, dislikes,
//! blocklist, slowcounts, crash breadcrumb), and render<->HTTP command channel.

use crate::config::{AppConfig, Settings};
use serde::Serialize;
use std::collections::{HashMap, HashSet, VecDeque};
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use walkdir::WalkDir;

pub const MAX_HISTORY: usize = 100;
pub const PREVIEW_EXPIRE: Duration = Duration::from_secs(5);
pub const PREVIEW_MIN_INTERVAL: Duration = Duration::from_millis(160);

#[derive(Clone, Debug, Serialize)]
pub struct AudioDeviceInfo {
    pub id: i32,
    pub name: String,
}

#[derive(Clone, Debug, Default, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct TelemetrySnapshot {
    pub cpu: u32,
    pub mem_used: u64,
    pub mem_total: u64,
    pub temp: u32,
    pub has_temp: bool,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct StatusSnapshot {
    pub preset: String,
    pub position: usize,
    pub size: usize,
    pub shuffle: bool,
    pub locked: bool,
    pub favorited: bool,
    pub favorites_shuffle: bool,
    pub category_shuffle: String,
    pub workshop: bool,
    pub blocked: usize,
    pub disliked: usize,
    pub catalog_revision: u64,
    pub fps: u32,
    pub cpu: u32,
    pub mem_used: u64,
    pub mem_total: u64,
    pub temp: u32,
    pub has_temp: bool,
    pub audio: String,
    pub audio_id: i32,
}

#[derive(Clone, Debug)]
pub enum Command {
    Next { hard: bool },
    Prev { hard: bool },
    Random { hard: bool },
    SelectIndex { index: usize, hard: bool },
    ToggleShuffle,
    ToggleLock,
    SwitchPack(String),
    ToggleFavoritesShuffle,
    ToggleCategoryShuffle,
    DislikeCurrent,
    ClearDislikes,
    ClearBlocklist,
    ApplyWorkshop { path: PathBuf, text: String },
    LoadWorkshopFile(PathBuf),
    SelectAudio(i32),
    NextAudio,
    ApplySettings,
}

#[derive(Default)]
pub struct PreviewSlot {
    pub bmp: Option<Vec<u8>>,
    pub seq: u64,
    pub captured_at: Option<Instant>,
    pub last_requested_at: Option<Instant>,
}

impl PreviewSlot {
    pub fn should_capture(&self, now: Instant, enabled: bool) -> bool {
        if !enabled {
            return false;
        }
        let Some(req) = self.last_requested_at else {
            return false;
        };
        if now.duration_since(req) > PREVIEW_EXPIRE {
            return false;
        }
        match self.captured_at {
            None => true,
            Some(cap) => now.duration_since(cap) >= PREVIEW_MIN_INTERVAL,
        }
    }

    pub fn store_frame(&mut self, bmp: Vec<u8>, now: Instant) {
        self.bmp = Some(bmp);
        self.seq = self.seq.wrapping_add(1);
        self.captured_at = Some(now);
    }

    pub fn take_fresh(&mut self, now: Instant) -> Option<(Vec<u8>, u64)> {
        self.last_requested_at = Some(now);
        let cap = self.captured_at?;
        if now.duration_since(cap) > PREVIEW_EXPIRE {
            self.bmp = None;
            return None;
        }
        self.bmp.clone().map(|b| (b, self.seq))
    }

    pub fn clear(&mut self) {
        self.bmp = None;
        self.captured_at = None;
    }
}

/// Persisted preset lists (`favorites.json`, `dislikes.txt`, `blocklist.txt`, `slowcounts.txt`)
/// and active playlist navigation.
pub struct PlaylistState {
    pub data_dir: PathBuf,
    pub preset_root: PathBuf,
    pub active_pack_dir: PathBuf,
    pub workshop_dir: PathBuf,

    pub items: Vec<String>,
    pub catalog_revision: u64,
    pub position: usize,
    pub history: Vec<usize>,

    pub shuffle: bool,
    pub locked: bool,
    pub favorites_shuffle: bool,
    pub category_shuffle: String,
    pub workshop_active: bool,
    pub workshop_path: String,

    pub favorites: HashSet<String>,
    pub dislikes: HashSet<String>,
    pub blocklist: HashSet<String>,
    pub slow_counts: HashMap<String, u32>,
}

impl PlaylistState {
    pub fn new(cfg: &AppConfig) -> Self {
        let _ = fs::create_dir_all(&cfg.data_dir);
        let _ = fs::create_dir_all(cfg.data_dir.join("state"));
        let _ = fs::create_dir_all(&cfg.workshop_dir);

        let favorites = load_favorites_json(&cfg.data_dir.join("favorites.json"));
        let dislikes = load_line_set(&cfg.data_dir.join("dislikes.txt"));
        let mut blocklist = load_line_set(&cfg.data_dir.join("blocklist.txt"));
        let slow_counts = load_slow_counts(&cfg.data_dir.join("slowcounts.txt"));

        // Crash breadcrumb check: if `state/loading` survived an unclean exit, quarantine it.
        let breadcrumb = cfg.data_dir.join("state").join("loading");
        if let Ok(prev) = fs::read_to_string(&breadcrumb) {
            let crashed = prev.trim().to_string();
            if !crashed.is_empty() && !blocklist.contains(&crashed) {
                log::warn!("Quarantining preset from unclean previous run: {crashed}");
                blocklist.insert(crashed.clone());
                let _ = append_line(&cfg.data_dir.join("blocklist.txt"), &crashed);
            }
            let _ = fs::remove_file(&breadcrumb);
        }

        let mut s = Self {
            data_dir: cfg.data_dir.clone(),
            preset_root: cfg.preset_root.clone(),
            active_pack_dir: cfg.preset_path.clone(),
            workshop_dir: cfg.workshop_dir.clone(),
            items: Vec::new(),
            catalog_revision: 0,
            position: 0,
            history: Vec::new(),
            shuffle: cfg.shuffle_enabled,
            locked: cfg.preset_locked,
            favorites_shuffle: false,
            category_shuffle: String::new(),
            workshop_active: false,
            workshop_path: String::new(),
            favorites,
            dislikes,
            blocklist,
            slow_counts,
        };
        s.reload_items();
        s
    }

    pub fn reload_items(&mut self) {
        let mut found = Vec::new();
        if self.active_pack_dir.is_dir() {
            for entry in WalkDir::new(&self.active_pack_dir)
                .follow_links(true)
                .into_iter()
                .filter_map(|e| e.ok())
            {
                if entry.file_type().is_file() {
                    let ext = entry.path().extension().and_then(|e| e.to_str()).unwrap_or("");
                    if ext.eq_ignore_ascii_case("milk") || ext.eq_ignore_ascii_case("milk2") {
                        let norm = normalize_path(entry.path());
                        if !self.dislikes.contains(&norm) && !self.blocklist.contains(&norm) {
                            found.push(norm);
                        }
                    }
                }
            }
        }
        found.sort();
        found.shrink_to_fit();
        self.items = found;
        self.catalog_revision = self.catalog_revision.wrapping_add(1);
        if self.position >= self.items.len() {
            self.position = 0;
        }
        self.history.retain(|&i| i < self.items.len());
    }

    pub fn current_path(&self) -> String {
        if self.workshop_active {
            self.workshop_path.clone()
        } else {
            self.items.get(self.position).cloned().unwrap_or_default()
        }
    }

    pub fn list_packs(&self) -> Vec<String> {
        let mut packs = Vec::new();
        if let Ok(rd) = fs::read_dir(&self.preset_root) {
            for entry in rd.flatten() {
                if entry.file_type().map(|t| t.is_dir()).unwrap_or(false) {
                    if let Some(name) = entry.file_name().to_str() {
                        if !name.starts_with('.') {
                            packs.push(name.to_string());
                        }
                    }
                }
            }
        }
        packs.sort();
        if packs.is_empty() {
            if let Some(name) = self.active_pack_dir.file_name().and_then(|s| s.to_str()) {
                packs.push(name.to_string());
            }
        }
        packs
    }

    pub fn switch_pack(&mut self, name: &str) -> Result<(), &'static str> {
        if name.is_empty() || name.contains("..") || name.contains('/') || name.contains('\\') {
            return Err("invalid pack name");
        }
        let dir = self.preset_root.join(name);
        if !dir.is_dir() {
            return Err("unknown pack");
        }
        self.active_pack_dir = dir;
        self.position = 0;
        self.history.clear();
        self.category_shuffle.clear();
        self.workshop_active = false;
        self.reload_items();
        Ok(())
    }

    pub fn move_to(&mut self, index: usize) -> Option<String> {
        if index >= self.items.len() {
            return None;
        }
        if index != self.position && !self.items.is_empty() {
            self.history.push(self.position);
            if self.history.len() > MAX_HISTORY {
                self.history.remove(0);
            }
        }
        if !self.category_shuffle.is_empty()
            && !self.items[index].starts_with(&self.category_shuffle)
        {
            self.category_shuffle.clear();
        }
        self.position = index;
        self.workshop_active = false;
        Some(self.items[index].clone())
    }

    pub fn jump(&mut self, in_order: bool) -> Option<String> {
        if self.items.is_empty() {
            return None;
        }
        if self.favorites_shuffle {
            let favs: Vec<usize> = self
                .items
                .iter()
                .enumerate()
                .filter(|(_, p)| self.favorites.contains(*p))
                .map(|(i, _)| i)
                .collect();
            if favs.is_empty() {
                return None;
            }
            let target = if in_order {
                favs.iter().copied().find(|&i| i > self.position).unwrap_or(favs[0])
            } else {
                pick_other(&favs, self.position)
            };
            return self.move_to(target);
        }
        if !self.category_shuffle.is_empty() {
            let cat: Vec<usize> = self
                .items
                .iter()
                .enumerate()
                .filter(|(_, p)| p.starts_with(&self.category_shuffle))
                .map(|(i, _)| i)
                .collect();
            if cat.is_empty() {
                self.category_shuffle.clear();
                return None;
            }
            let target = pick_other(&cat, self.position);
            return self.move_to(target);
        }
        let target = if in_order && !self.shuffle {
            (self.position + 1) % self.items.len()
        } else if in_order {
            (self.position + 1) % self.items.len()
        } else {
            let n = self.items.len();
            if n == 1 {
                0
            } else {
                let mut r = fastrand::usize(..n);
                if r == self.position {
                    r = (r + 1) % n;
                }
                r
            }
        };
        self.move_to(target)
    }

    pub fn next_auto(&mut self) -> Option<String> {
        self.jump(!self.shuffle)
    }

    pub fn prev(&mut self) -> Option<String> {
        if self.items.is_empty() {
            return None;
        }
        self.workshop_active = false;
        if let Some(prev_idx) = self.history.pop() {
            if prev_idx < self.items.len() {
                self.position = prev_idx;
                return Some(self.items[prev_idx].clone());
            }
        }
        self.position = (self.position + self.items.len() - 1) % self.items.len();
        Some(self.items[self.position].clone())
    }

    pub fn toggle_favorite(&mut self, path: &str) -> bool {
        let on = if self.favorites.remove(path) {
            false
        } else {
            self.favorites.insert(path.to_string());
            true
        };
        let _ = save_favorites_json(&self.data_dir.join("favorites.json"), &self.favorites);
        on
    }

    pub fn dislike_current(&mut self) -> Option<String> {
        if self.workshop_active {
            return None;
        }
        let cur = self.items.get(self.position)?.clone();
        if self.dislikes.insert(cur.clone()) {
            let _ = append_line(&self.data_dir.join("dislikes.txt"), &cur);
        }
        self.favorites.remove(&cur);
        let _ = save_favorites_json(&self.data_dir.join("favorites.json"), &self.favorites);
        self.items.remove(self.position);
        self.catalog_revision = self.catalog_revision.wrapping_add(1);
        if self.position >= self.items.len() {
            self.position = 0;
        }
        self.history.retain(|&i| i < self.items.len());
        self.items.get(self.position).cloned()
    }

    pub fn quarantine(&mut self, path: &str) -> Option<String> {
        if self.blocklist.insert(path.to_string()) {
            let _ = append_line(&self.data_dir.join("blocklist.txt"), path);
        }
        if let Some(idx) = self.items.iter().position(|p| p == path) {
            self.items.remove(idx);
            self.catalog_revision = self.catalog_revision.wrapping_add(1);
            if self.position >= self.items.len() {
                self.position = 0;
            }
            self.history.retain(|&i| i < self.items.len());
        }
        self.items.get(self.position).cloned()
    }

    pub fn record_slow_strike(&mut self, path: &str, max_strikes: u32) -> bool {
        if max_strikes == 0 {
            return false;
        }
        let count = self.slow_counts.entry(path.to_string()).or_insert(0);
        *count += 1;
        let hit = *count >= max_strikes;
        let _ = save_slow_counts(&self.data_dir.join("slowcounts.txt"), &self.slow_counts);
        if hit {
            let _ = self.quarantine(path);
        }
        hit
    }

    pub fn clear_dislikes(&mut self) {
        self.dislikes.clear();
        let _ = write_file_atomic(&self.data_dir.join("dislikes.txt"), b"");
        self.reload_items();
    }

    pub fn clear_blocklist(&mut self) {
        self.blocklist.clear();
        self.slow_counts.clear();
        let _ = write_file_atomic(&self.data_dir.join("blocklist.txt"), b"");
        let _ = write_file_atomic(&self.data_dir.join("slowcounts.txt"), b"");
        self.reload_items();
    }

    pub fn write_breadcrumb(&self, path: &str) {
        let p = self.data_dir.join("state").join("loading");
        let _ = fs::write(p, path.as_bytes());
    }

    pub fn clear_breadcrumb(&self) {
        let p = self.data_dir.join("state").join("loading");
        let _ = fs::remove_file(p);
    }
}

pub struct SharedState {
    pub playlist: Mutex<PlaylistState>,
    pub settings: Mutex<Settings>,
    pub telemetry: Mutex<TelemetrySnapshot>,
    pub fps: Mutex<u32>,
    pub audio_devices: Mutex<(i32, Vec<AudioDeviceInfo>)>,
    pub commands: Mutex<VecDeque<Command>>,
    pub preview: Mutex<PreviewSlot>,
    pub web_root: PathBuf,
    pub remote_token: String,
}

impl SharedState {
    pub fn new(cfg: &AppConfig, devices: Vec<AudioDeviceInfo>, current_audio: i32) -> Arc<Self> {
        Arc::new(Self {
            playlist: Mutex::new(PlaylistState::new(cfg)),
            settings: Mutex::new(cfg.settings.clone()),
            telemetry: Mutex::new(TelemetrySnapshot::default()),
            fps: Mutex::new(cfg.settings.fps),
            audio_devices: Mutex::new((current_audio, devices)),
            commands: Mutex::new(VecDeque::new()),
            preview: Mutex::new(PreviewSlot::default()),
            web_root: cfg.web_root.clone(),
            remote_token: cfg.remote_token.clone(),
        })
    }

    pub fn push_cmd(&self, cmd: Command) {
        self.commands.lock().unwrap().push_back(cmd);
    }

    pub fn drain_cmds(&self) -> Vec<Command> {
        self.commands.lock().unwrap().drain(..).collect()
    }

    pub fn status_snapshot(&self) -> StatusSnapshot {
        let pl = self.playlist.lock().unwrap();
        let tel = self.telemetry.lock().unwrap().clone();
        let fps = *self.fps.lock().unwrap();
        let (audio_id, ref devs) = *self.audio_devices.lock().unwrap();
        let audio = devs
            .iter()
            .find(|d| d.id == audio_id)
            .map(|d| d.name.clone())
            .unwrap_or_default();
        let preset = pl.current_path();
        StatusSnapshot {
            favorited: pl.favorites.contains(&preset),
            preset,
            position: pl.position,
            size: pl.items.len(),
            shuffle: pl.shuffle,
            locked: pl.locked,
            favorites_shuffle: pl.favorites_shuffle,
            category_shuffle: pl.category_shuffle.clone(),
            workshop: pl.workshop_active,
            blocked: pl.blocklist.len(),
            disliked: pl.dislikes.len(),
            catalog_revision: pl.catalog_revision,
            fps,
            cpu: tel.cpu,
            mem_used: tel.mem_used,
            mem_total: tel.mem_total,
            temp: tel.temp,
            has_temp: tel.has_temp,
            audio,
            audio_id,
        }
    }
}

fn pick_other(list: &[usize], current: usize) -> usize {
    if list.len() <= 1 {
        return list[0];
    }
    let idx = fastrand::usize(..list.len());
    if list[idx] == current {
        list[(idx + 1) % list.len()]
    } else {
        list[idx]
    }
}

pub fn normalize_path(path: &Path) -> String {
    path.to_string_lossy().replace('\\', "/")
}

pub fn write_file_atomic(dest: &Path, bytes: &[u8]) -> std::io::Result<()> {
    if let Some(parent) = dest.parent() {
        fs::create_dir_all(parent)?;
    }
    let tmp = dest.with_extension("tmp");
    {
        let mut f = fs::File::create(&tmp)?;
        f.write_all(bytes)?;
        f.sync_all()?;
    }
    if fs::rename(&tmp, dest).is_err() {
        // Windows rename fails if destination already exists on older FS layers
        let _ = fs::remove_file(dest);
        fs::rename(&tmp, dest)?;
    }
    Ok(())
}

fn append_line(path: &Path, line: &str) -> std::io::Result<()> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    let mut f = fs::OpenOptions::new().create(true).append(true).open(path)?;
    writeln!(f, "{line}")
}

fn load_line_set(path: &Path) -> HashSet<String> {
    fs::read_to_string(path)
        .unwrap_or_default()
        .lines()
        .map(str::trim)
        .filter(|l| !l.is_empty())
        .map(ToOwned::to_owned)
        .collect()
}

fn load_favorites_json(path: &Path) -> HashSet<String> {
    fs::read_to_string(path)
        .ok()
        .and_then(|s| serde_json::from_str::<Vec<String>>(&s).ok())
        .map(|v| v.into_iter().collect())
        .unwrap_or_default()
}

fn save_favorites_json(path: &Path, favs: &HashSet<String>) -> std::io::Result<()> {
    let mut sorted: Vec<&String> = favs.iter().collect();
    sorted.sort();
    let json = serde_json::to_vec_pretty(&sorted)?;
    write_file_atomic(path, &json)
}

fn load_slow_counts(path: &Path) -> HashMap<String, u32> {
    let mut map = HashMap::new();
    for line in fs::read_to_string(path).unwrap_or_default().lines() {
        if let Some((cnt, p)) = line.split_once('\t') {
            if let Ok(n) = cnt.trim().parse::<u32>() {
                map.insert(p.trim().to_string(), n);
            }
        }
    }
    map
}

fn save_slow_counts(path: &Path, map: &HashMap<String, u32>) -> std::io::Result<()> {
    let mut out = String::new();
    let mut entries: Vec<(&String, &u32)> = map.iter().collect();
    entries.sort_by(|a, b| a.0.cmp(b.0));
    for (p, n) in entries {
        out.push_str(&format!("{n}\t{p}\n"));
    }
    write_file_atomic(path, out.as_bytes())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn atomic_persistence_and_breadcrumb_quarantine() {
        let dir = std::env::temp_dir().join(format!("dk-state-test-{}", fastrand::u64(..)));
        let pack = dir.join("presets").join("pack1");
        fs::create_dir_all(&pack).unwrap();
        let p1 = pack.join("a.milk");
        let p2 = pack.join("b.milk");
        fs::write(&p1, "[preset00]\nfDecay=0.98\n").unwrap();
        fs::write(&p2, "[preset00]\nfDecay=0.97\n").unwrap();

        // Simulate crash breadcrumb on a.milk
        fs::create_dir_all(dir.join("state")).unwrap();
        let p1_norm = normalize_path(&p1);
        fs::write(dir.join("state").join("loading"), &p1_norm).unwrap();

        let mut cfg = AppConfig::load_from_args_and_properties();
        cfg.data_dir = dir.clone();
        cfg.preset_root = dir.join("presets");
        cfg.preset_path = pack.clone();
        cfg.workshop_dir = dir.join("workshop");

        let mut pl = PlaylistState::new(&cfg);
        assert!(pl.blocklist.contains(&p1_norm));
        assert_eq!(pl.items.len(), 1);
        assert_eq!(pl.items[0], normalize_path(&p2));

        let on = pl.toggle_favorite(&pl.items[0].clone());
        assert!(on);
        assert_eq!(load_favorites_json(&dir.join("favorites.json")).len(), 1);

        pl.clear_blocklist();
        assert_eq!(pl.items.len(), 2);

        let _ = fs::remove_dir_all(&dir);
    }
}
