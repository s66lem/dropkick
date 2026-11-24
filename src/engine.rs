//! Core Dropkick visualizer engine: drives `lactea::Engine`, manages preset loading,
//! transitions, workshop live apply, 4-second frame watchdog, low-FPS strike auto-skip,
//! audio PCM feeding, post-processing, and on-demand `/api/preview` capture.

use crate::audio::AudioCapture;
use crate::config::AppConfig;
use crate::postprocess::PostProcessor;
use crate::state::{Command, SharedState, normalize_path};
use lactea::{Compat, Engine, EngineConfig, FrameClock, Transition};
use std::fs;
use std::path::Path;
use std::sync::Arc;
use std::time::{Duration, Instant};

const WATCHDOG_LIMIT: Duration = Duration::from_secs(4);
const AUTOSKIP_GRACE: Duration = Duration::from_secs(2);
const AUTOSKIP_SUSTAIN: Duration = Duration::from_millis(1500);

pub struct DropkickEngine {
    engine: Engine,
    post: PostProcessor,
    audio: AudioCapture,
    pcm_scratch: Vec<f32>,
    state: Arc<SharedState>,

    width: u32,
    height: u32,
    start_time: Instant,
    frame_count: u32,
    preset_loaded_at: Instant,
    low_fps_since: Option<Instant>,

    fps_window_start: Instant,
    fps_window_frames: u32,
    measured_fps: f64,
}

impl DropkickEngine {
    pub fn new(
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        surface_format: wgpu::TextureFormat,
        cfg: &AppConfig,
        audio: AudioCapture,
        state: Arc<SharedState>,
    ) -> Result<Self, String> {
        let engine = Engine::new(
            device,
            queue,
            EngineConfig {
                width: cfg.width.max(1),
                height: cfg.height.max(1),
                compat: Compat::ProjectM,
                auto_compat: true,
                output_format: wgpu::TextureFormat::Rgba8Unorm,
                fps: cfg.settings.fps,
                preset_duration: cfg.settings.preset_duration,
                ..EngineConfig::default()
            },
        )
        .map_err(|e| format!("lactea::Engine::new failed: {e}"))?;

        let post = PostProcessor::new(device, cfg.width.max(1), cfg.height.max(1), surface_format);
        let now = Instant::now();

        let mut dk = Self {
            engine,
            post,
            audio,
            pcm_scratch: Vec::with_capacity(4096),
            state,
            width: cfg.width.max(1),
            height: cfg.height.max(1),
            start_time: now,
            frame_count: 0,
            preset_loaded_at: now,
            low_fps_since: None,
            fps_window_start: now,
            fps_window_frames: 0,
            measured_fps: cfg.settings.fps as f64,
        };

        // Load initial preset (or starter/idle preset if pack is empty)
        let first = dk.state.playlist.lock().unwrap().current_path();
        if !first.is_empty() {
            dk.load_preset_path(&first, true);
        } else {
            let _ = dk.engine.load_preset(
                lactea::IDLE_PRESET.as_bytes(),
                lactea::IDLE_PRESET_NAME,
                Transition::Cut,
            );
        }

        Ok(dk)
    }

    pub fn resize(&mut self, device: &wgpu::Device, width: u32, height: u32) {
        let w = width.max(1);
        let h = height.max(1);
        if w == self.width && h == self.height {
            return;
        }
        self.width = w;
        self.height = h;
        if let Err(e) = self.engine.resize(w, h) {
            log::warn!("lactea resize failed: {e}");
        }
        self.post.resize(device, w, h);
    }

    pub fn shutdown(&self) {
        self.state.playlist.lock().unwrap().clear_breadcrumb();
    }

    fn load_preset_path(&mut self, path: &str, hard: bool) {
        let transition = if hard {
            Transition::Cut
        } else {
            let sec = self.state.settings.lock().unwrap().soft_cut_duration;
            if sec <= 0.0 { Transition::Cut } else { Transition::Soft { seconds: sec } }
        };

        // Try up to 8 presets if a file is unreadable or has broken EEL syntax
        let mut candidate = path.to_string();
        for _ in 0..8 {
            let Ok(bytes) = fs::read(&candidate) else {
                log::warn!("Preset file missing: {candidate}");
                if let Some(next) = self.state.playlist.lock().unwrap().next_auto() {
                    candidate = next;
                    continue;
                }
                return;
            };
            let name = Path::new(&candidate)
                .file_name()
                .map(|s| s.to_string_lossy().to_string())
                .unwrap_or_else(|| candidate.clone());

            self.state.playlist.lock().unwrap().write_breadcrumb(&candidate);
            match self.engine.load_preset(&bytes, &name, transition) {
                Ok(_report) => {
                    self.preset_loaded_at = Instant::now();
                    self.low_fps_since = None;
                    return;
                }
                Err(e) => {
                    log::warn!("Skipping preset {name}: {e}");
                    let next = self.state.playlist.lock().unwrap().next_auto();
                    if let Some(n) = next {
                        if n != candidate {
                            candidate = n;
                            continue;
                        }
                    }
                    return;
                }
            }
        }
    }

    fn load_workshop_bytes(&mut self, path: &Path, bytes: &[u8]) {
        let norm = normalize_path(path);
        let name = path
            .file_name()
            .map(|s| s.to_string_lossy().to_string())
            .unwrap_or_else(|| "_scratch.milk".to_string());
        match self.engine.load_preset(bytes, &name, Transition::Cut) {
            Ok(_) => {
                let mut pl = self.state.playlist.lock().unwrap();
                pl.workshop_active = true;
                pl.workshop_path = norm;
                self.preset_loaded_at = Instant::now();
                self.low_fps_since = None;
            }
            Err(e) => log::warn!("Workshop preset error ({name}): {e}"),
        }
    }

    fn drain_commands(&mut self) {
        let cmds = self.state.drain_cmds();
        for cmd in cmds {
            match cmd {
                Command::Next { hard } => {
                    let next = self.state.playlist.lock().unwrap().jump(true);
                    if let Some(p) = next { self.load_preset_path(&p, hard); }
                }
                Command::Prev { hard } => {
                    let prev = self.state.playlist.lock().unwrap().prev();
                    if let Some(p) = prev { self.load_preset_path(&p, hard); }
                }
                Command::Random { hard } => {
                    let rand = self.state.playlist.lock().unwrap().jump(false);
                    if let Some(p) = rand { self.load_preset_path(&p, hard); }
                }
                Command::SelectIndex { index, hard } => {
                    let picked = self.state.playlist.lock().unwrap().move_to(index);
                    if let Some(p) = picked { self.load_preset_path(&p, hard); }
                }
                Command::ToggleShuffle => {
                    let mut pl = self.state.playlist.lock().unwrap();
                    pl.shuffle = !pl.shuffle;
                }
                Command::ToggleLock => {
                    let mut pl = self.state.playlist.lock().unwrap();
                    pl.locked = !pl.locked;
                }
                Command::SwitchPack(name) => {
                    let first = {
                        let mut pl = self.state.playlist.lock().unwrap();
                        if pl.switch_pack(&name).is_ok() {
                            pl.items.first().cloned()
                        } else {
                            None
                        }
                    };
                    if let Some(p) = first {
                        self.load_preset_path(&p, true);
                    }
                }
                Command::ToggleFavoritesShuffle => {
                    let enabled = self.state.playlist.lock().unwrap().favorites_shuffle;
                    if enabled {
                        let next = self.state.playlist.lock().unwrap().jump(false);
                        if let Some(p) = next { self.load_preset_path(&p, false); }
                    }
                }
                Command::ToggleCategoryShuffle => {
                    let next = self.state.playlist.lock().unwrap().jump(false);
                    if let Some(p) = next { self.load_preset_path(&p, false); }
                }
                Command::DislikeCurrent => {
                    let next = self.state.playlist.lock().unwrap().dislike_current();
                    if let Some(p) = next { self.load_preset_path(&p, false); }
                }
                Command::ClearDislikes => {
                    self.state.playlist.lock().unwrap().clear_dislikes();
                }
                Command::ClearBlocklist => {
                    self.state.playlist.lock().unwrap().clear_blocklist();
                }
                Command::ApplyWorkshop { path, text } => {
                    self.load_workshop_bytes(&path, text.as_bytes());
                }
                Command::LoadWorkshopFile(path) => {
                    if let Ok(bytes) = fs::read(&path) {
                        self.load_workshop_bytes(&path, &bytes);
                    }
                }
                Command::SelectAudio(id) => {
                    if self.audio.select(id).is_ok() {
                        *self.state.audio_devices.lock().unwrap() = self.audio.list();
                    }
                }
                Command::NextAudio => {
                    if self.audio.next_device().is_ok() {
                        *self.state.audio_devices.lock().unwrap() = self.audio.list();
                    }
                }
                Command::ApplySettings => {
                    let s = self.state.settings.lock().unwrap().clone();
                    self.engine.set_fps(s.fps);
                    self.engine.set_preset_duration(s.preset_duration);
                    self.engine.set_volume_sensitivity(s.beat_sensitivity as f32);
                }
            }
        }
    }

    /// Renders one frame: drains remote commands, feeds audio PCM, runs `lactea` + `PostProcessor`,
    /// handles auto-rotation, 4s frame watchdog, low-FPS strike auto-skip, and on-demand `/api/preview`.
    pub fn render_frame(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        surface_target: Option<&wgpu::TextureView>,
    ) {
        self.drain_commands();

        let settings = self.state.settings.lock().unwrap().clone();
        let now = Instant::now();

        // Auto-rotate preset when unlocked and duration elapsed
        let (locked, workshop) = {
            let pl = self.state.playlist.lock().unwrap();
            (pl.locked, pl.workshop_active)
        };
        if !locked && !workshop && now.duration_since(self.preset_loaded_at).as_secs_f64() >= settings.preset_duration {
            let next = self.state.playlist.lock().unwrap().next_auto();
            if let Some(p) = next {
                self.load_preset_path(&p, false);
            }
        }

        // Feed live stereo PCM from audio capture (scaled by beat sensitivity)
        self.audio.pcm().drain_into(&mut self.pcm_scratch);
        if !self.pcm_scratch.is_empty() {
            let gain = settings.beat_sensitivity as f32;
            if (gain - 1.0).abs() > 1e-3 {
                for s in &mut self.pcm_scratch {
                    *s = (*s * gain).clamp(-1.0, 1.0);
                }
            }
            self.engine.push_pcm_f32(&self.pcm_scratch, 2);
        }

        let want_preview = self
            .state
            .preview
            .lock()
            .unwrap()
            .should_capture(now, settings.live_preview);

        let frame_start = Instant::now();
        let mut encoder = device.create_command_encoder(&wgpu::CommandEncoderDescriptor {
            label: Some("dropkick-frame"),
        });

        let clock = FrameClock {
            time: now.duration_since(self.start_time).as_secs_f64(),
            frame: self.frame_count,
        };
        self.engine.render(&mut encoder, self.post.scene_view(), clock);
        self.post.apply(queue, &mut encoder, &settings, surface_target, want_preview);
        queue.submit(Some(encoder.finish()));

        if want_preview {
            if let Some(bmp) = self.post.read_preview_bmp(device) {
                self.state.preview.lock().unwrap().store_frame(bmp, Instant::now());
            }
        }

        let frame_elapsed = frame_start.elapsed();
        self.frame_count = self.frame_count.wrapping_add(1);
        self.fps_window_frames += 1;
        let win_dt = now.duration_since(self.fps_window_start).as_secs_f64();
        if win_dt >= 0.5 {
            self.measured_fps = self.fps_window_frames as f64 / win_dt;
            *self.state.fps.lock().unwrap() = self.measured_fps.round() as u32;
            self.fps_window_start = now;
            self.fps_window_frames = 0;
        }

        // Frame watchdog (>4s single frame -> quarantine)
        if !workshop && frame_elapsed >= WATCHDOG_LIMIT {
            let cur = self.state.playlist.lock().unwrap().current_path();
            log::warn!("Frame watchdog triggered ({:.2}s) on {cur}; quarantining", frame_elapsed.as_secs_f64());
            let next = self.state.playlist.lock().unwrap().quarantine(&cur);
            if let Some(p) = next {
                self.load_preset_path(&p, true);
            }
            return;
        }

        // Low-FPS strike auto-skip (paused while post-processing effects are active)
        let post_active = settings.reduce_flashing || settings.tint_enabled || (settings.brightness - 1.0).abs() > 0.01;
        if settings.autoskip_enabled && !workshop && !locked && !post_active {
            if now.duration_since(self.preset_loaded_at) >= AUTOSKIP_GRACE {
                if self.measured_fps < settings.autoskip_fps {
                    let since = *self.low_fps_since.get_or_insert(now);
                    if now.duration_since(since) >= AUTOSKIP_SUSTAIN {
                        let cur = self.state.playlist.lock().unwrap().current_path();
                        let strikes = settings.autoskip_strikes;
                        let next = {
                            let mut pl = self.state.playlist.lock().unwrap();
                            pl.record_slow_strike(&cur, strikes);
                            pl.next_auto()
                        };
                        self.low_fps_since = None;
                        if let Some(p) = next {
                            self.load_preset_path(&p, true);
                        }
                    }
                } else {
                    self.low_fps_since = None;
                }
            }
        }
    }
}
