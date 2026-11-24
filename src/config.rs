//! Configuration, CLI arguments, and validated/clamped runtime settings.

use serde::Serialize;
use std::collections::HashMap;
use std::path::{Path, PathBuf};

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Settings {
    pub preset_duration: f64,
    pub soft_cut_duration: f64,
    pub hard_cut: bool,
    pub hard_cut_duration: f64,
    pub hard_cut_sensitivity: f64,
    pub beat_sensitivity: f64,
    pub fps: u32,
    pub aspect_correction: bool,
    pub reduce_flashing: bool,
    pub flash_strength: f32,
    pub brightness: f32,
    pub tint_enabled: bool,
    pub tint_color: String,
    pub tint_strength: f32,
    pub live_preview: bool,
    pub autoskip_enabled: bool,
    pub autoskip_fps: f64,
    pub autoskip_strikes: u32,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            preset_duration: 30.0,
            soft_cut_duration: 3.0,
            hard_cut: false,
            hard_cut_duration: 20.0,
            hard_cut_sensitivity: 2.0,
            beat_sensitivity: 1.0,
            fps: 60,
            aspect_correction: true,
            reduce_flashing: false,
            flash_strength: 0.6,
            brightness: 1.0,
            tint_enabled: false,
            tint_color: "#00ff00".to_string(),
            tint_strength: 1.0,
            live_preview: true,
            autoskip_enabled: true,
            autoskip_fps: 20.0,
            autoskip_strikes: 3,
        }
    }
}

impl Settings {
    /// Parses, validates, clamps, and applies a single `POST /api/settings` key/value pair.
    /// Mirrors `PresetLists::ClampSettingValue` / `RemoteControl::ParseSetting` exactly.
    pub fn apply_kv(&mut self, key: &str, raw_value: &str) -> Result<(), &'static str> {
        if raw_value.is_empty() {
            return Err("empty value");
        }
        if key == "tintColor" {
            let hex = raw_value.strip_prefix('#').unwrap_or(raw_value);
            if hex.len() != 6 || !hex.bytes().all(|b| b.is_ascii_hexdigit()) {
                return Err("invalid hex color");
            }
            self.tint_color = format!("#{}", hex.to_ascii_lowercase());
            return Ok(());
        }

        let parse_bool = |s: &str| -> Result<bool, &'static str> {
            match s {
                "true" => Ok(true),
                "false" => Ok(false),
                _ => {
                    if s.trim() != s {
                        return Err("whitespace in numeric value");
                    }
                    let n: f64 = s.parse().map_err(|_| "invalid boolean")?;
                    if !n.is_finite() {
                        return Err("non-finite value");
                    }
                    Ok(n != 0.0)
                }
            }
        };

        let parse_f64 = |s: &str, lo: f64, hi: f64| -> Result<f64, &'static str> {
            if s.trim() != s {
                return Err("whitespace in numeric value");
            }
            let n: f64 = s.parse().map_err(|_| "invalid number")?;
            if !n.is_finite() {
                return Err("non-finite value");
            }
            Ok(n.clamp(lo, hi))
        };

        match key {
            "presetDuration" => self.preset_duration = parse_f64(raw_value, 1.0, 3600.0)?,
            "softCutDuration" => self.soft_cut_duration = parse_f64(raw_value, 0.0, 30.0)?,
            "hardCut" => self.hard_cut = parse_bool(raw_value)?,
            "hardCutDuration" => self.hard_cut_duration = parse_f64(raw_value, 0.0, 300.0)?,
            "hardCutSensitivity" => self.hard_cut_sensitivity = parse_f64(raw_value, 0.0, 10.0)?,
            "beatSensitivity" => self.beat_sensitivity = parse_f64(raw_value, 0.0, 5.0)?,
            "fps" => self.fps = parse_f64(raw_value, 10.0, 240.0)?.round() as u32,
            "aspectCorrection" => self.aspect_correction = parse_bool(raw_value)?,
            "reduceFlashing" => self.reduce_flashing = parse_bool(raw_value)?,
            "flashStrength" => self.flash_strength = parse_f64(raw_value, 0.0, 1.0)? as f32,
            "brightness" => self.brightness = parse_f64(raw_value, 0.05, 1.0)? as f32,
            "tintEnabled" => self.tint_enabled = parse_bool(raw_value)?,
            "tintStrength" => self.tint_strength = parse_f64(raw_value, 0.0, 1.0)? as f32,
            "livePreview" => self.live_preview = parse_bool(raw_value)?,
            "autoskipEnabled" => self.autoskip_enabled = parse_bool(raw_value)?,
            "autoskipFps" => self.autoskip_fps = parse_f64(raw_value, 1.0, 120.0)?,
            "autoskipStrikes" => self.autoskip_strikes = parse_f64(raw_value, 0.0, 50.0)?.round() as u32,
            _ => return Err("unknown setting"),
        }
        Ok(())
    }

    pub fn tint_rgb(&self) -> [f32; 3] {
        let hex = self.tint_color.strip_prefix('#').unwrap_or(&self.tint_color);
        if hex.len() == 6 {
            if let Ok(v) = u32::from_str_radix(hex, 16) {
                return [
                    ((v >> 16) & 0xff) as f32 / 255.0,
                    ((v >> 8) & 0xff) as f32 / 255.0,
                    (v & 0xff) as f32 / 255.0,
                ];
            }
        }
        [0.0, 1.0, 0.0]
    }
}

#[derive(Clone, Debug)]
pub struct AppConfig {
    pub width: u32,
    pub height: u32,
    pub fullscreen: bool,
    pub headless: bool,
    pub preset_path: PathBuf,
    pub preset_root: PathBuf,
    pub texture_path: PathBuf,
    pub web_root: PathBuf,
    pub workshop_dir: PathBuf,
    pub data_dir: PathBuf,
    pub remote_host: String,
    pub remote_port: u16,
    pub remote_token: String,
    pub shuffle_enabled: bool,
    pub preset_locked: bool,
    pub audio_device: String,
    pub settings: Settings,
    pub no_audio: bool,
}

impl AppConfig {
    pub fn load_from_args_and_properties() -> Self {
        let args: Vec<String> = std::env::args().collect();
        let home = std::env::var_os("USERPROFILE")
            .or_else(|| std::env::var_os("HOME"))
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from("."));
        let default_data = home.join(".local").join("share").join("dropkick");

        // Locate optional properties file
        let mut props_path = None;
        for i in 1..args.len() {
            if args[i] == "--config" && i + 1 < args.len() {
                props_path = Some(PathBuf::from(&args[i + 1]));
            }
        }
        if props_path.is_none() {
            if let Ok(exe) = std::env::current_exe() {
                if let Some(dir) = exe.parent() {
                    let cand = dir.join("projectMSDL.properties");
                    if cand.exists() {
                        props_path = Some(cand);
                    }
                }
            }
        }
        let props = props_path
            .as_deref()
            .and_then(|p| parse_properties_file(p).ok())
            .unwrap_or_default();

        let get_str = |k: &str| props.get(k).cloned().unwrap_or_default();
        let get_bool = |k: &str, def: bool| -> bool {
            match props.get(k).map(|s| s.to_ascii_lowercase()).as_deref() {
                Some("true" | "1" | "yes") => true,
                Some("false" | "0" | "no") => false,
                _ => def,
            }
        };

        let mut cfg = Self {
            width: props.get("window.width").and_then(|s| s.parse().ok()).unwrap_or(1280),
            height: props.get("window.height").and_then(|s| s.parse().ok()).unwrap_or(720),
            fullscreen: get_bool("window.fullscreen", false),
            headless: false,
            preset_root: non_empty_path(&get_str("remote.presetRoot"))
                .unwrap_or_else(|| default_data.join("presets")),
            preset_path: non_empty_path(&get_str("projectM.presetPath"))
                .unwrap_or_else(|| default_data.join("presets").join("cream-of-the-crop")),
            texture_path: non_empty_path(&get_str("projectM.texturePath"))
                .unwrap_or_else(|| default_data.join("textures")),
            web_root: non_empty_path(&get_str("remote.webRoot"))
                .unwrap_or_else(|| {
                    let local = PathBuf::from("remote");
                    if local.is_dir() { local } else { default_data.join("remote") }
                }),
            workshop_dir: non_empty_path(&get_str("remote.workshopDir"))
                .unwrap_or_else(|| default_data.join("workshop")),
            data_dir: default_data,
            remote_host: props.get("remote.host").cloned().unwrap_or_else(|| "0.0.0.0".to_string()),
            remote_port: props.get("remote.port").and_then(|s| s.parse().ok()).unwrap_or(8080),
            remote_token: props.get("remote.token").cloned().unwrap_or_default(),
            shuffle_enabled: get_bool("projectM.shuffleEnabled", true),
            preset_locked: get_bool("projectM.presetLocked", false),
            audio_device: get_str("audio.device"),
            settings: Settings::default(),
            no_audio: false,
        };

        // Map known properties into Settings
        let prop_to_setting = [
            ("projectM.displayDuration", "presetDuration"),
            ("projectM.transitionDuration", "softCutDuration"),
            ("projectM.hardCutsEnabled", "hardCut"),
            ("projectM.hardCutDuration", "hardCutDuration"),
            ("projectM.hardCutSensitivity", "hardCutSensitivity"),
            ("projectM.beatSensitivity", "beatSensitivity"),
            ("projectM.fps", "fps"),
            ("projectM.aspectCorrectionEnabled", "aspectCorrection"),
            ("projectM.reduceFlashing", "reduceFlashing"),
            ("projectM.flashStrength", "flashStrength"),
            ("projectM.brightness", "brightness"),
            ("projectM.tintEnabled", "tintEnabled"),
            ("projectM.tintColor", "tintColor"),
            ("projectM.tintStrength", "tintStrength"),
            ("projectM.livePreview", "livePreview"),
            ("projectM.autoskipEnabled", "autoskipEnabled"),
            ("projectM.autoskipFps", "autoskipFps"),
            ("projectM.autoskipStrikes", "autoskipStrikes"),
        ];
        for (pk, sk) in prop_to_setting {
            if let Some(v) = props.get(pk) {
                let _ = cfg.settings.apply_kv(sk, v);
            }
        }

        // CLI overrides
        let mut i = 1;
        while i < args.len() {
            let arg = args[i].as_str();
            let next = args.get(i + 1).map(|s| s.as_str());
            match (arg, next) {
                ("--headless", _) => { cfg.headless = true; i += 1; }
                ("--fullscreen", _) => { cfg.fullscreen = true; i += 1; }
                ("--no-audio", _) => { cfg.no_audio = true; i += 1; }
                ("--port", Some(v)) => { if let Ok(p) = v.parse() { cfg.remote_port = p; } i += 2; }
                ("--host", Some(v)) => { cfg.remote_host = v.to_string(); i += 2; }
                ("--token", Some(v)) => { cfg.remote_token = v.to_string(); i += 2; }
                ("--presets", Some(v)) => { cfg.preset_path = PathBuf::from(v); i += 2; }
                ("--preset-root", Some(v)) => { cfg.preset_root = PathBuf::from(v); i += 2; }
                ("--textures", Some(v)) => { cfg.texture_path = PathBuf::from(v); i += 2; }
                ("--web-root", Some(v)) => { cfg.web_root = PathBuf::from(v); i += 2; }
                ("--data-dir", Some(v)) => {
                    cfg.data_dir = PathBuf::from(v);
                    cfg.workshop_dir = cfg.data_dir.join("workshop");
                    i += 2;
                }
                ("--workshop-dir", Some(v)) => { cfg.workshop_dir = PathBuf::from(v); i += 2; }
                ("--width", Some(v)) => { if let Ok(w) = v.parse() { cfg.width = w; } i += 2; }
                ("--height", Some(v)) => { if let Ok(h) = v.parse() { cfg.height = h; } i += 2; }
                ("--fps", Some(v)) => { let _ = cfg.settings.apply_kv("fps", v); i += 2; }
                ("--config", Some(_)) => { i += 2; }
                _ => { i += 1; }
            }
        }
        cfg
    }
}

fn non_empty_path(s: &str) -> Option<PathBuf> {
    let t = s.trim();
    if t.is_empty() { None } else { Some(PathBuf::from(t)) }
}

pub fn parse_properties_file(path: &Path) -> std::io::Result<HashMap<String, String>> {
    let text = std::fs::read_to_string(path)?;
    Ok(parse_properties_str(&text))
}

pub fn parse_properties_str(text: &str) -> HashMap<String, String> {
    let mut map = HashMap::new();
    for line in text.lines() {
        let trimmed = line.trim();
        if trimmed.is_empty() || trimmed.starts_with('#') || trimmed.starts_with('!') {
            continue;
        }
        if let Some((k, v)) = trimmed.split_once('=') {
            map.insert(k.trim().to_string(), v.trim().to_string());
        }
    }
    map
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn settings_clamping_and_validation() {
        let mut s = Settings::default();
        assert!(s.apply_kv("brightness", "0.0").is_ok());
        assert!((s.brightness - 0.05).abs() < 1e-6);
        assert!(s.apply_kv("brightness", "99").is_ok());
        assert!((s.brightness - 1.0).abs() < 1e-6);
        assert!(s.apply_kv("fps", "999").is_ok());
        assert_eq!(s.fps, 240);
        assert!(s.apply_kv("fps", "2").is_ok());
        assert_eq!(s.fps, 10);
        assert!(s.apply_kv("tintColor", "ff40c0").is_ok());
        assert_eq!(s.tint_color, "#ff40c0");
        assert!(s.apply_kv("tintColor", "nothex").is_err());
        assert!(s.apply_kv("unknownKey", "1").is_err());
        assert!(s.apply_kv("fps", "NaN").is_err());
        assert!(s.apply_kv("fps", " 60").is_err());
        assert!(s.apply_kv("livePreview", "false").is_ok());
        assert!(!s.live_preview);
        assert!(s.apply_kv("livePreview", "1").is_ok());
        assert!(s.live_preview);
    }

    #[test]
    fn parses_properties_ignoring_comments() {
        let p = parse_properties_str("# comment\nremote.port = 9090\nremote.token = op1\n! ignored\n");
        assert_eq!(p.get("remote.port").map(String::as_str), Some("9090"));
        assert_eq!(p.get("remote.token").map(String::as_str), Some("op1"));
    }
}
