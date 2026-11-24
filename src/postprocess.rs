//! Single-pass wgpu post-processing pipeline (Reduce Flashing temporal persistence ping-pong,
//! Brightness, Monochrome Screen Tint) plus on-demand 240x150 live preview capture.

use crate::bmp::{PREVIEW_HEIGHT, PREVIEW_WIDTH, encode_rgba_top_down_to_bmp24};
use crate::config::Settings;
use bytemuck::{Pod, Zeroable};

#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
struct PostUniforms {
    brightness: f32,
    persistence: f32,
    tint_enabled: f32,
    tint_strength: f32,
    tint_r: f32,
    tint_g: f32,
    tint_b: f32,
    _pad: f32,
}

const POST_WGSL: &str = r#"
struct Uniforms {
    brightness: f32,
    persistence: f32,
    tint_enabled: f32,
    tint_strength: f32,
    tint_r: f32,
    tint_g: f32,
    tint_b: f32,
    _pad: f32,
};

@group(0) @binding(0) var u_scene: texture_2d<f32>;
@group(0) @binding(1) var u_prev: texture_2d<f32>;
@group(0) @binding(2) var u_sampler: sampler;
@group(0) @binding(3) var<uniform> u: Uniforms;

struct VsOut {
    @builtin(position) pos: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) vid: u32) -> VsOut {
    let x = f32((vid << 1u) & 2u);
    let y = f32(vid & 2u);
    var out: VsOut;
    out.pos = vec4<f32>(x * 2.0 - 1.0, 1.0 - y * 2.0, 0.0, 1.0);
    out.uv = vec2<f32>(x, y);
    return out;
}

struct FsOut {
    @location(0) display: vec4<f32>,
    @location(1) smoothed: vec4<f32>,
};

@fragment
fn fs_main(in: VsOut) -> FsOut {
    var smoothed = textureSample(u_scene, u_sampler, in.uv).rgb;
    if (u.persistence > 0.0) {
        let prev = textureSample(u_prev, u_sampler, in.uv).rgb;
        smoothed = mix(smoothed, prev, u.persistence);
    }
    var color = smoothed * u.brightness;
    if (u.tint_enabled > 0.5) {
        let lum = dot(color, vec3<f32>(0.2126, 0.7152, 0.0722));
        let tint = vec3<f32>(u.tint_r, u.tint_g, u.tint_b);
        color = mix(color, lum * tint, u.tint_strength);
    }
    var out: FsOut;
    out.display = vec4<f32>(clamp(color, vec3<f32>(0.0), vec3<f32>(1.0)), 1.0);
    out.smoothed = vec4<f32>(smoothed, 1.0);
    return out;
}

@fragment
fn fs_display_only(in: VsOut) -> @location(0) vec4<f32> {
    var color = textureSample(u_scene, u_sampler, in.uv).rgb * u.brightness;
    if (u.tint_enabled > 0.5) {
        let lum = dot(color, vec3<f32>(0.2126, 0.7152, 0.0722));
        let tint = vec3<f32>(u.tint_r, u.tint_g, u.tint_b);
        color = mix(color, lum * tint, u.tint_strength);
    }
    return vec4<f32>(clamp(color, vec3<f32>(0.0), vec3<f32>(1.0)), 1.0);
}
"#;

const BLIT_WGSL: &str = r#"
@group(0) @binding(0) var u_tex: texture_2d<f32>;
@group(0) @binding(1) var u_sampler: sampler;

struct VsOut {
    @builtin(position) pos: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) vid: u32) -> VsOut {
    let x = f32((vid << 1u) & 2u);
    let y = f32(vid & 2u);
    var out: VsOut;
    out.pos = vec4<f32>(x * 2.0 - 1.0, 1.0 - y * 2.0, 0.0, 1.0);
    out.uv = vec2<f32>(x, y);
    return out;
}

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4<f32> {
    return textureSample(u_tex, u_sampler, in.uv);
}
"#;

struct DisplayTargets {
    _tex: wgpu::Texture,
    view: wgpu::TextureView,
    blit_bg: wgpu::BindGroup,
    simple_bg: wgpu::BindGroup,
}

struct HistoryTargets {
    _textures: [wgpu::Texture; 2],
    views: [wgpu::TextureView; 2],
    post_bgs: [wgpu::BindGroup; 2],
}

pub struct PostProcessor {
    width: u32,
    height: u32,
    _scene_tex: wgpu::Texture,
    scene_view: wgpu::TextureView,
    scene_blit_bg: wgpu::BindGroup,

    // Lazily allocated only while post-processing effects are active, keeping idle VRAM/RSS small.
    display: Option<DisplayTargets>,
    history: Option<HistoryTargets>,
    hist_idx: usize,
    history_valid: bool,

    sampler: wgpu::Sampler,
    uniform_buf: wgpu::Buffer,
    post_bgl: wgpu::BindGroupLayout,
    post_mrt_pipeline: wgpu::RenderPipeline,
    post_simple_pipeline: wgpu::RenderPipeline,

    blit_bgl: wgpu::BindGroupLayout,
    surface_blit_pipeline: wgpu::RenderPipeline,

    // On-demand 240x150 preview downscale + staging buffer
    preview_tex: wgpu::Texture,
    preview_view: wgpu::TextureView,
    preview_pipeline: wgpu::RenderPipeline,
    preview_staging: wgpu::Buffer,
    preview_bytes_per_row: u32,
}

impl PostProcessor {
    pub fn new(
        device: &wgpu::Device,
        width: u32,
        height: u32,
        surface_format: wgpu::TextureFormat,
    ) -> Self {
        let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("dropkick-linear-sampler"),
            mag_filter: wgpu::FilterMode::Linear,
            min_filter: wgpu::FilterMode::Linear,
            ..Default::default()
        });

        let uniform_buf = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("dropkick-post-uniforms"),
            size: std::mem::size_of::<PostUniforms>() as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });

        let post_bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("dropkick-post-bgl"),
            entries: &[
                tex_entry(0),
                tex_entry(1),
                wgpu::BindGroupLayoutEntry {
                    binding: 2,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 3,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: None,
                    },
                    count: None,
                },
            ],
        });

        let post_shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("dropkick-post-wgsl"),
            source: wgpu::ShaderSource::Wgsl(POST_WGSL.into()),
        });
        let post_pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("dropkick-post-pl"),
            bind_group_layouts: &[Some(&post_bgl)],
            immediate_size: 0,
        });
        let post_mrt_pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("dropkick-post-mrt-pipeline"),
            layout: Some(&post_pl),
            vertex: wgpu::VertexState {
                module: &post_shader,
                entry_point: Some("vs_main"),
                buffers: &[],
                compilation_options: Default::default(),
            },
            fragment: Some(wgpu::FragmentState {
                module: &post_shader,
                entry_point: Some("fs_main"),
                targets: &[
                    Some(wgpu::ColorTargetState {
                        format: wgpu::TextureFormat::Rgba8Unorm,
                        blend: None,
                        write_mask: wgpu::ColorWrites::ALL,
                    }),
                    Some(wgpu::ColorTargetState {
                        format: wgpu::TextureFormat::Rgba8Unorm,
                        blend: None,
                        write_mask: wgpu::ColorWrites::ALL,
                    }),
                ],
                compilation_options: Default::default(),
            }),
            primitive: Default::default(),
            depth_stencil: None,
            multisample: Default::default(),
            multiview_mask: None,
            cache: None,
        });
        let post_simple_pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("dropkick-post-simple-pipeline"),
            layout: Some(&post_pl),
            vertex: wgpu::VertexState {
                module: &post_shader,
                entry_point: Some("vs_main"),
                buffers: &[],
                compilation_options: Default::default(),
            },
            fragment: Some(wgpu::FragmentState {
                module: &post_shader,
                entry_point: Some("fs_display_only"),
                targets: &[Some(wgpu::ColorTargetState {
                    format: wgpu::TextureFormat::Rgba8Unorm,
                    blend: None,
                    write_mask: wgpu::ColorWrites::ALL,
                })],
                compilation_options: Default::default(),
            }),
            primitive: Default::default(),
            depth_stencil: None,
            multisample: Default::default(),
            multiview_mask: None,
            cache: None,
        });

        let blit_bgl = device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("dropkick-blit-bgl"),
            entries: &[
                tex_entry(0),
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
            ],
        });
        let blit_shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("dropkick-blit-wgsl"),
            source: wgpu::ShaderSource::Wgsl(BLIT_WGSL.into()),
        });
        let blit_pl = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("dropkick-blit-pl"),
            bind_group_layouts: &[Some(&blit_bgl)],
            immediate_size: 0,
        });

        let make_blit = |label: &str, fmt: wgpu::TextureFormat| {
            device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some(label),
                layout: Some(&blit_pl),
                vertex: wgpu::VertexState {
                    module: &blit_shader,
                    entry_point: Some("vs_main"),
                    buffers: &[],
                    compilation_options: Default::default(),
                },
                fragment: Some(wgpu::FragmentState {
                    module: &blit_shader,
                    entry_point: Some("fs_main"),
                    targets: &[Some(wgpu::ColorTargetState {
                        format: fmt,
                        blend: None,
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                    compilation_options: Default::default(),
                }),
                primitive: Default::default(),
                depth_stencil: None,
                multisample: Default::default(),
                multiview_mask: None,
                cache: None,
            })
        };

        let surface_blit_pipeline = make_blit("dropkick-surface-blit", surface_format);
        let preview_pipeline = make_blit("dropkick-preview-blit", wgpu::TextureFormat::Rgba8Unorm);

        let preview_tex = create_rgba_tex(device, "dropkick-preview", PREVIEW_WIDTH, PREVIEW_HEIGHT, true);
        let preview_view = preview_tex.create_view(&Default::default());
        let unpadded = PREVIEW_WIDTH * 4;
        let align = wgpu::COPY_BYTES_PER_ROW_ALIGNMENT;
        let preview_bytes_per_row = (unpadded + align - 1) & !(align - 1);
        let preview_staging = device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("dropkick-preview-staging"),
            size: (preview_bytes_per_row * PREVIEW_HEIGHT) as u64,
            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
            mapped_at_creation: false,
        });

        let w = width.max(1);
        let h = height.max(1);
        let (scene_tex, scene_view, scene_blit_bg) = create_scene_target(device, w, h, &sampler, &blit_bgl);

        Self {
            width: w,
            height: h,
            _scene_tex: scene_tex,
            scene_view,
            scene_blit_bg,
            display: None,
            history: None,
            hist_idx: 0,
            history_valid: false,
            sampler,
            uniform_buf,
            post_bgl,
            post_mrt_pipeline,
            post_simple_pipeline,
            blit_bgl,
            surface_blit_pipeline,
            preview_tex,
            preview_view,
            preview_pipeline,
            preview_staging,
            preview_bytes_per_row,
        }
    }

    pub fn resize(&mut self, device: &wgpu::Device, width: u32, height: u32) {
        let w = width.max(1);
        let h = height.max(1);
        if w == self.width && h == self.height {
            return;
        }
        self.width = w;
        self.height = h;
        let (scene_tex, scene_view, scene_blit_bg) =
            create_scene_target(device, w, h, &self.sampler, &self.blit_bgl);
        self._scene_tex = scene_tex;
        self.scene_view = scene_view;
        self.scene_blit_bg = scene_blit_bg;
        // Drop lazy targets; they will reallocate at the new dimensions if still needed.
        self.display = None;
        self.history = None;
        self.history_valid = false;
    }

    pub fn scene_view(&self) -> &wgpu::TextureView {
        &self.scene_view
    }

    /// Runs the post-process pass only when brightness/tint/reduce-flashing is active, otherwise
    /// blits `scene_tex` directly (saving 3 full-res textures and a full-screen render pass).
    pub fn apply(
        &mut self,
        device: &wgpu::Device,
        queue: &wgpu::Queue,
        encoder: &mut wgpu::CommandEncoder,
        settings: &Settings,
        surface_target: Option<&wgpu::TextureView>,
        capture_preview: bool,
    ) {
        let brightness = settings.brightness.clamp(0.05, 1.0);
        let need_post = settings.reduce_flashing || settings.tint_enabled || (brightness - 1.0).abs() > 0.001;

        if !need_post {
            self.display = None;
            self.history = None;
            self.history_valid = false;
        } else {
            if self.display.is_none() {
                self.display = Some(create_display_target(
                    device,
                    self.width,
                    self.height,
                    &self.scene_view,
                    &self.sampler,
                    &self.uniform_buf,
                    &self.post_bgl,
                    &self.blit_bgl,
                ));
            }
            if settings.reduce_flashing {
                if self.history.is_none() {
                    self.history = Some(create_history_targets(
                        device,
                        self.width,
                        self.height,
                        &self.scene_view,
                        &self.sampler,
                        &self.uniform_buf,
                        &self.post_bgl,
                    ));
                    self.history_valid = false;
                }
            } else {
                self.history = None;
                self.history_valid = false;
            }

            let persistence = if settings.reduce_flashing && self.history_valid {
                // Map flashStrength (0..1) into smooth persistence (0..0.88) so motion stays responsive
                (settings.flash_strength.clamp(0.0, 1.0) * 0.88).clamp(0.0, 0.88)
            } else {
                0.0
            };
            let rgb = settings.tint_rgb();
            let u = PostUniforms {
                brightness,
                persistence,
                tint_enabled: if settings.tint_enabled { 1.0 } else { 0.0 },
                tint_strength: settings.tint_strength.clamp(0.0, 1.0),
                tint_r: rgb[0],
                tint_g: rgb[1],
                tint_b: rgb[2],
                _pad: 0.0,
            };
            queue.write_buffer(&self.uniform_buf, 0, bytemuck::bytes_of(&u));

            let disp = self.display.as_ref().unwrap();
            if let Some(hist) = self.history.as_ref() {
                let read_idx = self.hist_idx;
                let write_idx = 1 - self.hist_idx;
                {
                    let mut rp = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                        label: Some("dropkick-post-mrt-pass"),
                        color_attachments: &[
                            Some(wgpu::RenderPassColorAttachment {
                                view: &disp.view,
                                depth_slice: None,
                                resolve_target: None,
                                ops: wgpu::Operations {
                                    load: wgpu::LoadOp::Clear(wgpu::Color::BLACK),
                                    store: wgpu::StoreOp::Store,
                                },
                            }),
                            Some(wgpu::RenderPassColorAttachment {
                                view: &hist.views[write_idx],
                                depth_slice: None,
                                resolve_target: None,
                                ops: wgpu::Operations {
                                    load: wgpu::LoadOp::Clear(wgpu::Color::BLACK),
                                    store: wgpu::StoreOp::Store,
                                },
                            }),
                        ],
                        depth_stencil_attachment: None,
                        timestamp_writes: None,
                        occlusion_query_set: None,
                        multiview_mask: None,
                    });
                    rp.set_pipeline(&self.post_mrt_pipeline);
                    rp.set_bind_group(0, &hist.post_bgs[read_idx], &[]);
                    rp.draw(0..3, 0..1);
                }
                self.hist_idx = write_idx;
                self.history_valid = true;
            } else {
                let mut rp = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                    label: Some("dropkick-post-simple-pass"),
                    color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                        view: &disp.view,
                        depth_slice: None,
                        resolve_target: None,
                        ops: wgpu::Operations {
                            load: wgpu::LoadOp::Clear(wgpu::Color::BLACK),
                            store: wgpu::StoreOp::Store,
                        },
                    })],
                    depth_stencil_attachment: None,
                    timestamp_writes: None,
                    occlusion_query_set: None,
                    multiview_mask: None,
                });
                rp.set_pipeline(&self.post_simple_pipeline);
                rp.set_bind_group(0, &disp.simple_bg, &[]);
                rp.draw(0..3, 0..1);
            }
        }

        let source_blit_bg = self
            .display
            .as_ref()
            .map(|d| &d.blit_bg)
            .unwrap_or(&self.scene_blit_bg);

        if let Some(target) = surface_target {
            let mut rp = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("dropkick-surface-blit-pass"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: target,
                    depth_slice: None,
                    resolve_target: None,
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Clear(wgpu::Color::BLACK),
                        store: wgpu::StoreOp::Store,
                    },
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
                multiview_mask: None,
            });
            rp.set_pipeline(&self.surface_blit_pipeline);
            rp.set_bind_group(0, source_blit_bg, &[]);
            rp.draw(0..3, 0..1);
        }

        if capture_preview {
            {
                let mut rp = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                    label: Some("dropkick-preview-blit-pass"),
                    color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                        view: &self.preview_view,
                        depth_slice: None,
                        resolve_target: None,
                        ops: wgpu::Operations {
                            load: wgpu::LoadOp::Clear(wgpu::Color::BLACK),
                            store: wgpu::StoreOp::Store,
                        },
                    })],
                    depth_stencil_attachment: None,
                    timestamp_writes: None,
                    occlusion_query_set: None,
                    multiview_mask: None,
                });
                rp.set_pipeline(&self.preview_pipeline);
                rp.set_bind_group(0, source_blit_bg, &[]);
                rp.draw(0..3, 0..1);
            }
            encoder.copy_texture_to_buffer(
                wgpu::TexelCopyTextureInfo {
                    texture: &self.preview_tex,
                    mip_level: 0,
                    origin: wgpu::Origin3d::ZERO,
                    aspect: wgpu::TextureAspect::All,
                },
                wgpu::TexelCopyBufferInfo {
                    buffer: &self.preview_staging,
                    layout: wgpu::TexelCopyBufferLayout {
                        offset: 0,
                        bytes_per_row: Some(self.preview_bytes_per_row),
                        rows_per_image: Some(PREVIEW_HEIGHT),
                    },
                },
                wgpu::Extent3d {
                    width: PREVIEW_WIDTH,
                    height: PREVIEW_HEIGHT,
                    depth_or_array_layers: 1,
                },
            );
        }
    }

    /// Reads back the staged 240x150 preview texture after queue submission and encodes 24-bit BMP.
    pub fn read_preview_bmp(&self, device: &wgpu::Device) -> Option<Vec<u8>> {
        let slice = self.preview_staging.slice(..);
        let (tx, rx) = std::sync::mpsc::channel();
        slice.map_async(wgpu::MapMode::Read, move |res| {
            let _ = tx.send(res);
        });
        let _ = device.poll(wgpu::PollType::wait_indefinitely());
        rx.recv().ok()?.ok()?;
        let data = slice.get_mapped_range().ok()?;
        let row_unpadded = (PREVIEW_WIDTH * 4) as usize;
        let stride = self.preview_bytes_per_row as usize;
        let mut tightly_packed = vec![0u8; row_unpadded * PREVIEW_HEIGHT as usize];
        for y in 0..PREVIEW_HEIGHT as usize {
            let src = y * stride;
            let dst = y * row_unpadded;
            tightly_packed[dst..dst + row_unpadded].copy_from_slice(&data[src..src + row_unpadded]);
        }
        drop(data);
        self.preview_staging.unmap();
        encode_rgba_top_down_to_bmp24(&tightly_packed, PREVIEW_WIDTH, PREVIEW_HEIGHT)
    }
}

fn tex_entry(binding: u32) -> wgpu::BindGroupLayoutEntry {
    wgpu::BindGroupLayoutEntry {
        binding,
        visibility: wgpu::ShaderStages::FRAGMENT,
        ty: wgpu::BindingType::Texture {
            sample_type: wgpu::TextureSampleType::Float { filterable: true },
            view_dimension: wgpu::TextureViewDimension::D2,
            multisampled: false,
        },
        count: None,
    }
}

fn create_rgba_tex(
    device: &wgpu::Device,
    label: &str,
    width: u32,
    height: u32,
    copy_src: bool,
) -> wgpu::Texture {
    let mut usage = wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::TEXTURE_BINDING;
    if copy_src {
        usage |= wgpu::TextureUsages::COPY_SRC;
    }
    device.create_texture(&wgpu::TextureDescriptor {
        label: Some(label),
        size: wgpu::Extent3d { width, height, depth_or_array_layers: 1 },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu::TextureFormat::Rgba8Unorm,
        usage,
        view_formats: &[],
    })
}

fn create_scene_target(
    device: &wgpu::Device,
    width: u32,
    height: u32,
    sampler: &wgpu::Sampler,
    blit_bgl: &wgpu::BindGroupLayout,
) -> (wgpu::Texture, wgpu::TextureView, wgpu::BindGroup) {
    let scene_tex = create_rgba_tex(device, "dropkick-scene", width, height, false);
    let scene_view = scene_tex.create_view(&Default::default());
    let scene_blit_bg = device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some("dropkick-scene-blit-bg"),
        layout: blit_bgl,
        entries: &[
            wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(&scene_view) },
            wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::Sampler(sampler) },
        ],
    });
    (scene_tex, scene_view, scene_blit_bg)
}

#[allow(clippy::too_many_arguments)]
fn create_display_target(
    device: &wgpu::Device,
    width: u32,
    height: u32,
    scene_view: &wgpu::TextureView,
    sampler: &wgpu::Sampler,
    uniform_buf: &wgpu::Buffer,
    post_bgl: &wgpu::BindGroupLayout,
    blit_bgl: &wgpu::BindGroupLayout,
) -> DisplayTargets {
    let tex = create_rgba_tex(device, "dropkick-display", width, height, false);
    let view = tex.create_view(&Default::default());
    let blit_bg = device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some("dropkick-display-blit-bg"),
        layout: blit_bgl,
        entries: &[
            wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(&view) },
            wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::Sampler(sampler) },
        ],
    });
    let simple_bg = device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some("dropkick-post-simple-bg"),
        layout: post_bgl,
        entries: &[
            wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(scene_view) },
            wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::TextureView(scene_view) },
            wgpu::BindGroupEntry { binding: 2, resource: wgpu::BindingResource::Sampler(sampler) },
            wgpu::BindGroupEntry { binding: 3, resource: uniform_buf.as_entire_binding() },
        ],
    });
    DisplayTargets { _tex: tex, view, blit_bg, simple_bg }
}

fn create_history_targets(
    device: &wgpu::Device,
    width: u32,
    height: u32,
    scene_view: &wgpu::TextureView,
    sampler: &wgpu::Sampler,
    uniform_buf: &wgpu::Buffer,
    post_bgl: &wgpu::BindGroupLayout,
) -> HistoryTargets {
    let h0 = create_rgba_tex(device, "dropkick-hist-0", width, height, false);
    let h1 = create_rgba_tex(device, "dropkick-hist-1", width, height, false);
    let hv0 = h0.create_view(&Default::default());
    let hv1 = h1.create_view(&Default::default());
    let make_bg = |prev: &wgpu::TextureView| {
        device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("dropkick-post-bg"),
            layout: post_bgl,
            entries: &[
                wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(scene_view) },
                wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::TextureView(prev) },
                wgpu::BindGroupEntry { binding: 2, resource: wgpu::BindingResource::Sampler(sampler) },
                wgpu::BindGroupEntry { binding: 3, resource: uniform_buf.as_entire_binding() },
            ],
        })
    };
    let post_bgs = [make_bg(&hv0), make_bg(&hv1)];
    HistoryTargets { _textures: [h0, h1], views: [hv0, hv1], post_bgs }
}
