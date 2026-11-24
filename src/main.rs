//! Dropkick — Pure-Rust MilkDrop / projectM audio visualizer powered by Lactea + wgpu.
//! Supports windowed/fullscreen desktop display (Vulkan / DX12 / Metal) and `--headless`
//! offscreen GPU mode, with an embedded LAN remote web UI & live `/api/preview`.

mod audio;
mod bmp;
mod config;
mod engine;
mod monitor;
mod postprocess;
mod remote;
mod state;

use crate::audio::AudioCapture;
use crate::config::AppConfig;
use crate::engine::DropkickEngine;
use crate::state::{Command, SharedState};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};
use winit::application::ApplicationHandler;
use winit::dpi::PhysicalSize;
use winit::event::{ElementState, MouseButton, WindowEvent};
use winit::event_loop::{ActiveEventLoop, ControlFlow, EventLoop};
use winit::keyboard::{Key, ModifiersState, NamedKey};
use winit::window::{Fullscreen, Window, WindowId};

fn main() {
    let cfg = AppConfig::load_from_args_and_properties();
    let audio = AudioCapture::new(cfg.no_audio, &cfg.audio_device);
    let (audio_id, audio_list) = audio.list();
    let state = SharedState::new(&cfg, audio_list, audio_id);

    let stop = Arc::new(AtomicBool::new(false));
    let _mon = monitor::spawn_monitor(state.clone(), cfg.workshop_dir.clone(), stop.clone());

    let (bound_addr, _http_rt) = remote::spawn_http_server(
        state.clone(),
        cfg.remote_host.clone(),
        cfg.remote_port,
    )
    .expect("Failed to start Dropkick HTTP remote server");

    let tok_suffix = if cfg.remote_token.is_empty() {
        String::new()
    } else {
        format!("/{}", cfg.remote_token)
    };
    println!("Dropkick (Rust / Lactea + wgpu)");
    println!("  Remote:  http://{bound_addr}{tok_suffix}");
    println!("  Presets: {} ({} loaded)", cfg.preset_path.display(), state.playlist.lock().unwrap().items.len());

    if cfg.headless {
        run_headless(cfg, audio, state, stop);
    } else {
        let event_loop = EventLoop::new().expect("Failed to create event loop");
        event_loop.set_control_flow(ControlFlow::Poll);
        let mut app = WindowedApp {
            cfg,
            audio: Some(audio),
            state,
            stop: stop.clone(),
            gpu: None,
            modifiers: ModifiersState::empty(),
            last_frame: Instant::now(),
        };
        let _ = event_loop.run_app(&mut app);
        stop.store(true, Ordering::Relaxed);
    }
}

fn run_headless(
    cfg: AppConfig,
    audio: AudioCapture,
    state: Arc<SharedState>,
    stop: Arc<AtomicBool>,
) {
    let instance = wgpu::Instance::default();
    let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
        power_preference: wgpu::PowerPreference::HighPerformance,
        ..Default::default()
    }))
    .expect("No wgpu adapter available for headless rendering");
    let info = adapter.get_info();
    println!("  GPU:     {} ({:?}) [headless]", info.name, info.backend);

    let (device, queue) = pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor::default()))
        .expect("Failed to create wgpu device");

    let mut dk = DropkickEngine::new(
        &device,
        &queue,
        wgpu::TextureFormat::Rgba8Unorm,
        &cfg,
        audio,
        state.clone(),
    )
    .expect("Failed to initialize DropkickEngine");

    while !stop.load(Ordering::Relaxed) {
        let t0 = Instant::now();
        dk.render_frame(&device, &queue, None);
        let target_fps = state.settings.lock().unwrap().fps.max(10) as u64;
        let budget = Duration::from_nanos(1_000_000_000 / target_fps);
        if let Some(rem) = budget.checked_sub(t0.elapsed()) {
            std::thread::sleep(rem);
        }
    }
    dk.shutdown();
}

struct GpuState {
    window: Arc<Window>,
    surface: wgpu::Surface<'static>,
    surface_cfg: wgpu::SurfaceConfiguration,
    device: wgpu::Device,
    queue: wgpu::Queue,
    dk: DropkickEngine,
}

struct WindowedApp {
    cfg: AppConfig,
    audio: Option<AudioCapture>,
    state: Arc<SharedState>,
    stop: Arc<AtomicBool>,
    gpu: Option<GpuState>,
    modifiers: ModifiersState,
    last_frame: Instant,
}

impl ApplicationHandler for WindowedApp {
    fn resumed(&mut self, event_loop: &ActiveEventLoop) {
        if self.gpu.is_some() {
            return;
        }
        let mut attrs = Window::default_attributes()
            .with_title("Dropkick")
            .with_inner_size(PhysicalSize::new(self.cfg.width, self.cfg.height));
        if self.cfg.fullscreen {
            attrs = attrs.with_fullscreen(Some(Fullscreen::Borderless(None)));
        }
        let window = Arc::new(event_loop.create_window(attrs).expect("Failed to create window"));
        let size = window.inner_size();

        let instance = wgpu::Instance::default();
        let surface = instance
            .create_surface(window.clone())
            .expect("Failed to create wgpu surface");
        let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: wgpu::PowerPreference::HighPerformance,
            compatible_surface: Some(&surface),
            ..Default::default()
        }))
        .expect("No compatible GPU adapter found");
        let info = adapter.get_info();
        println!("  GPU:     {} ({:?})", info.name, info.backend);

        let (device, queue) = pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor::default()))
            .expect("Failed to create wgpu device");

        let caps = surface.get_capabilities(&adapter);
        let format = caps
            .formats
            .iter()
            .copied()
            .find(|f| !f.is_srgb())
            .unwrap_or(caps.formats[0]);
        let present_mode = if caps.present_modes.contains(&wgpu::PresentMode::Fifo) {
            wgpu::PresentMode::Fifo
        } else {
            caps.present_modes[0]
        };
        let surface_cfg = wgpu::SurfaceConfiguration {
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
            format,
            color_space: wgpu::SurfaceColorSpace::Auto,
            width: size.width.max(1),
            height: size.height.max(1),
            present_mode,
            alpha_mode: caps.alpha_modes[0],
            view_formats: vec![],
            desired_maximum_frame_latency: 2,
        };
        surface.configure(&device, &surface_cfg);

        let mut init_cfg = self.cfg.clone();
        init_cfg.width = surface_cfg.width;
        init_cfg.height = surface_cfg.height;
        let audio = self.audio.take().unwrap_or_else(|| AudioCapture::new(true, ""));
        let dk = DropkickEngine::new(&device, &queue, format, &init_cfg, audio, self.state.clone())
            .expect("Failed to initialize DropkickEngine");

        self.gpu = Some(GpuState { window, surface, surface_cfg, device, queue, dk });
    }

    fn window_event(&mut self, event_loop: &ActiveEventLoop, _id: WindowId, event: WindowEvent) {
        match event {
            WindowEvent::CloseRequested => {
                if let Some(g) = &self.gpu {
                    g.dk.shutdown();
                }
                self.stop.store(true, Ordering::Relaxed);
                event_loop.exit();
            }
            WindowEvent::Resized(new_size) => {
                if let Some(g) = &mut self.gpu {
                    if new_size.width > 0 && new_size.height > 0 {
                        g.surface_cfg.width = new_size.width;
                        g.surface_cfg.height = new_size.height;
                        g.surface.configure(&g.device, &g.surface_cfg);
                        g.dk.resize(&g.device, new_size.width, new_size.height);
                    }
                }
            }
            WindowEvent::ModifiersChanged(m) => {
                self.modifiers = m.state();
            }
            WindowEvent::MouseInput { state: ElementState::Pressed, button: MouseButton::Right, .. } => {
                self.toggle_fullscreen();
            }
            WindowEvent::KeyboardInput { event, .. } if event.state == ElementState::Pressed => {
                let ctrl = self.modifiers.control_key() || self.modifiers.super_key();
                let shift = self.modifiers.shift_key();
                match event.logical_key {
                    Key::Character(ref s) => match s.to_ascii_lowercase().as_str() {
                        "q" if ctrl => {
                            if let Some(g) = &self.gpu { g.dk.shutdown(); }
                            self.stop.store(true, Ordering::Relaxed);
                            event_loop.exit();
                        }
                        "f" if ctrl => self.toggle_fullscreen(),
                        "i" if ctrl => self.state.push_cmd(Command::NextAudio),
                        "n" => self.state.push_cmd(Command::Next { hard: shift }),
                        "p" => self.state.push_cmd(Command::Prev { hard: shift }),
                        "r" => self.state.push_cmd(Command::Random { hard: shift }),
                        "l" => self.state.push_cmd(Command::ToggleLock),
                        "y" => self.state.push_cmd(Command::ToggleShuffle),
                        "f" => {
                            let cur = self.state.playlist.lock().unwrap().current_path();
                            if !cur.is_empty() {
                                self.state.playlist.lock().unwrap().toggle_favorite(&cur);
                            }
                        }
                        "a" => {
                            let mut s = self.state.settings.lock().unwrap();
                            s.aspect_correction = !s.aspect_correction;
                        }
                        _ => {}
                    },
                    Key::Named(NamedKey::Space) => self.state.push_cmd(Command::ToggleLock),
                    Key::Named(NamedKey::Backspace) => self.state.push_cmd(Command::Prev { hard: shift }),
                    Key::Named(NamedKey::ArrowUp) => {
                        let mut s = self.state.settings.lock().unwrap();
                        s.beat_sensitivity = (s.beat_sensitivity + 0.05).min(5.0);
                        self.state.push_cmd(Command::ApplySettings);
                    }
                    Key::Named(NamedKey::ArrowDown) => {
                        let mut s = self.state.settings.lock().unwrap();
                        s.beat_sensitivity = (s.beat_sensitivity - 0.05).max(0.0);
                        self.state.push_cmd(Command::ApplySettings);
                    }
                    _ => {}
                }
            }
            WindowEvent::RedrawRequested => {
                if let Some(g) = &mut self.gpu {
                    match g.surface.get_current_texture() {
                        wgpu::CurrentSurfaceTexture::Success(frame)
                        | wgpu::CurrentSurfaceTexture::Suboptimal(frame) => {
                            let view = frame.texture.create_view(&Default::default());
                            g.dk.render_frame(&g.device, &g.queue, Some(&view));
                            g.queue.present(frame);
                        }
                        wgpu::CurrentSurfaceTexture::Outdated | wgpu::CurrentSurfaceTexture::Lost => {
                            g.surface.configure(&g.device, &g.surface_cfg);
                        }
                        _ => {}
                    }
                }
            }
            _ => {}
        }
    }

    fn about_to_wait(&mut self, _event_loop: &ActiveEventLoop) {
        let target_fps = self.state.settings.lock().unwrap().fps.max(10) as u64;
        let budget = Duration::from_nanos(1_000_000_000 / target_fps);
        if self.last_frame.elapsed() >= budget {
            self.last_frame = Instant::now();
            if let Some(g) = &self.gpu {
                g.window.request_redraw();
            }
        }
    }
}

impl WindowedApp {
    fn toggle_fullscreen(&self) {
        if let Some(g) = &self.gpu {
            let next = if g.window.fullscreen().is_some() {
                None
            } else {
                Some(Fullscreen::Borderless(None))
            };
            g.window.set_fullscreen(next);
        }
    }
}
