//! Cross-platform audio capture via `cpal` (Windows WASAPI loopback + system inputs on all OSes).

use crate::state::AudioDeviceInfo;
use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
use std::collections::VecDeque;
use std::sync::{Arc, Mutex};

const MAX_PCM_FLOATS: usize = 4096 * 2; // stereo ring buffer (~90 ms at 44.1 kHz)

#[derive(Clone)]
pub struct PcmBuffer {
    inner: Arc<Mutex<VecDeque<f32>>>,
}

impl PcmBuffer {
    pub fn new() -> Self {
        Self {
            inner: Arc::new(Mutex::new(VecDeque::with_capacity(MAX_PCM_FLOATS))),
        }
    }

    pub fn push_stereo(&self, samples: &[f32]) {
        if let Ok(mut q) = self.inner.lock() {
            for &s in samples {
                if q.len() >= MAX_PCM_FLOATS {
                    q.pop_front();
                }
                q.push_back(s);
            }
        }
    }

    pub fn drain_into(&self, out: &mut Vec<f32>) {
        out.clear();
        if let Ok(mut q) = self.inner.lock() {
            out.extend(q.drain(..));
        }
    }
}

struct DeviceEntry {
    id: i32,
    name: String,
    device: Option<cpal::Device>,
    is_loopback: bool,
}

pub struct AudioCapture {
    pcm: PcmBuffer,
    entries: Vec<DeviceEntry>,
    current_id: i32,
    stream: Option<cpal::Stream>,
    disabled: bool,
}

impl AudioCapture {
    pub fn new(disabled: bool, preferred: &str) -> Self {
        let pcm = PcmBuffer::new();
        let mut cap = Self {
            pcm,
            entries: Vec::new(),
            current_id: -1,
            stream: None,
            disabled,
        };
        cap.enumerate();
        if !disabled {
            if !preferred.is_empty() {
                if let Some(found) = cap
                    .entries
                    .iter()
                    .find(|e| e.name.to_ascii_lowercase().contains(&preferred.to_ascii_lowercase()))
                    .map(|e| e.id)
                {
                    let _ = cap.select(found);
                    return cap;
                }
            }
            let _ = cap.select(-1);
        }
        cap
    }

    pub fn pcm(&self) -> PcmBuffer {
        self.pcm.clone()
    }

    pub fn list(&self) -> (i32, Vec<AudioDeviceInfo>) {
        let list = self
            .entries
            .iter()
            .map(|e| AudioDeviceInfo { id: e.id, name: e.name.clone() })
            .collect();
        (self.current_id, list)
    }

    fn enumerate(&mut self) {
        self.entries.clear();
        self.entries.push(DeviceEntry {
            id: -1,
            name: "System Default".to_string(),
            device: None,
            is_loopback: true,
        });

        let host = cpal::default_host();
        let mut next_id = 0i32;

        // On Windows WASAPI, output devices can be opened as loopback capture streams.
        #[cfg(target_os = "windows")]
        if let Ok(outputs) = host.output_devices() {
            for dev in outputs {
                let name = dev.name().unwrap_or_else(|_| format!("Output {next_id}"));
                self.entries.push(DeviceEntry {
                    id: next_id,
                    name: format!("{name} (Loopback)"),
                    device: Some(dev),
                    is_loopback: true,
                });
                next_id += 1;
            }
        }

        if let Ok(inputs) = host.input_devices() {
            for dev in inputs {
                let name = dev.name().unwrap_or_else(|_| format!("Input {next_id}"));
                self.entries.push(DeviceEntry {
                    id: next_id,
                    name,
                    device: Some(dev),
                    is_loopback: false,
                });
                next_id += 1;
            }
        }
    }

    pub fn select(&mut self, id: i32) -> Result<String, String> {
        let idx = self
            .entries
            .iter()
            .position(|e| e.id == id)
            .ok_or_else(|| "unknown audio device".to_string())?;
        self.current_id = id;
        let name = self.entries[idx].name.clone();
        if self.disabled {
            return Ok(name);
        }

        self.stream = None;
        let host = cpal::default_host();
        let (device, is_loopback) = if id == -1 {
            #[cfg(target_os = "windows")]
            {
                if let Some(out) = host.default_output_device() {
                    (Some(out), true)
                } else {
                    (host.default_input_device(), false)
                }
            }
            #[cfg(not(target_os = "windows"))]
            {
                (host.default_input_device(), false)
            }
        } else {
            (self.entries[idx].device.clone(), self.entries[idx].is_loopback)
        };

        let Some(dev) = device else {
            log::warn!("No audio device available for id {id}");
            return Ok(name);
        };

        let config_res = if is_loopback {
            dev.default_output_config().or_else(|_| dev.default_input_config())
        } else {
            dev.default_input_config()
        };
        let Ok(cfg) = config_res else {
            log::warn!("Could not query audio config for {name}");
            return Ok(name);
        };

        let channels = cfg.channels() as usize;
        let sample_fmt = cfg.sample_format();
        let stream_cfg: cpal::StreamConfig = cfg.into();
        let pcm = self.pcm.clone();
        let err_fn = |err| log::debug!("audio stream error: {err}");

        let stream = match sample_fmt {
            cpal::SampleFormat::F32 => dev.build_input_stream(
                &stream_cfg,
                move |data: &[f32], _| push_interleaved(&pcm, data, channels),
                err_fn,
                None,
            ),
            cpal::SampleFormat::I16 => dev.build_input_stream(
                &stream_cfg,
                move |data: &[i16], _| {
                    let f: Vec<f32> = data.iter().map(|&s| s as f32 / 32768.0).collect();
                    push_interleaved(&pcm, &f, channels);
                },
                err_fn,
                None,
            ),
            cpal::SampleFormat::U16 => dev.build_input_stream(
                &stream_cfg,
                move |data: &[u16], _| {
                    let f: Vec<f32> = data.iter().map(|&s| (s as f32 - 32768.0) / 32768.0).collect();
                    push_interleaved(&pcm, &f, channels);
                },
                err_fn,
                None,
            ),
            _ => {
                log::warn!("Unsupported audio sample format {sample_fmt:?} on {name}");
                return Ok(name);
            }
        };

        match stream {
            Ok(s) => {
                if let Err(e) = s.play() {
                    log::warn!("Failed to start audio stream on {name}: {e}");
                } else {
                    self.stream = Some(s);
                }
            }
            Err(e) => log::warn!("Failed to open audio stream on {name}: {e}"),
        }
        Ok(name)
    }

    pub fn next_device(&mut self) -> Result<String, String> {
        if self.entries.is_empty() {
            return Ok(String::new());
        }
        let pos = self.entries.iter().position(|e| e.id == self.current_id).unwrap_or(0);
        let next_id = self.entries[(pos + 1) % self.entries.len()].id;
        self.select(next_id)
    }
}

fn push_interleaved(pcm: &PcmBuffer, data: &[f32], channels: usize) {
    if channels == 2 {
        pcm.push_stereo(data);
    } else if channels == 1 {
        let mut stereo = Vec::with_capacity(data.len() * 2);
        for &s in data {
            stereo.push(s);
            stereo.push(s);
        }
        pcm.push_stereo(&stereo);
    } else if channels > 2 {
        let frames = data.len() / channels;
        let mut stereo = Vec::with_capacity(frames * 2);
        for f in 0..frames {
            stereo.push(data[f * channels]);
            stereo.push(data[f * channels + 1]);
        }
        pcm.push_stereo(&stereo);
    }
}
