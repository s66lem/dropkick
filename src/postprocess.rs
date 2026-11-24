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

pub struct PostProcessor {
    width: u32,
    height: u32,
    scene_tex: wgpu::Texture,
    scene_view: wgpu::TextureView,
    display_tex: wgpu::Texture,
    display_view: wgpu::TextureView,
    history: [wgpu::Texture; 2],
    history_views: [wgpu::TextureView; 2],
    hist_idx: usize,
    history_valid: bool,

    sampler: wgpu::Sampler,
    uniform_buf: wgpu::Buffer,
    post_bgl: wgpu::BindGroupLayout,
    post_bgs: [wgpu::BindGroup; 2],
    post_pipeline: wgpu::RenderPipeline,

    blit_bgl: wgpu::BindGroupLayout,
    blit_bg: wgpu::BindGroup,
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
        let post_pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: Some("dropkick-post-pipeline"),
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

        let (scene_tex, scene_view, display_tex, display_view, history, history_views, post_bgs, blit_bg) =
            create_size_dependent(device, width.max(1), height.max(1), &sampler, &uniform_buf, &post_bgl, &blit_bgl);

        Self {
            width: width.max(1),
            height: height.max(1),
            scene_tex,
            scene_view,
            display_tex,
            display_view,
            history,
            history_views,
            hist_idx: 0,
            history_valid: false,
            sampler,
            uniform_buf,
            post_bgl,
            post_bgs,
            post_pipeline,
            blit_bgl,
            blit_bg,
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
        let (scene_tex, scene_view, display_tex, display_view, history, history_views, post_bgs, blit_bg) =
            create_size_dependent(device, w, h, &self.sampler, &self.uniform_buf, &self.post_bgl, &self.blit_bgl);
        self.scene_tex = scene_tex;
        self.scene_view = scene_view;
        self.display_tex = display_tex;
        self.display_view = display_view;
        self.history = history;
        self.history_views = history_views;
        self.post_bgs = post_bgs;
        self.blit_bg = blit_bg;
        self.history_valid = false;
    }

    pub fn scene_view(&self) -> &wgpu::TextureView {
        &self.scene_view
    }

    /// Runs the post-process MRT pass (`scene -> display + smoothed history`), optionally blits
    /// to a window surface, and optionally downscales to the 240x150 preview staging buffer.
    pub fn apply(
        &mut self,
        queue: &wgpu::Queue,
        encoder: &mut wgpu::CommandEncoder,
        settings: &Settings,
        surface_target: Option<&wgpu::TextureView>,
        capture_preview: bool,
    ) {
        let persistence = if settings.reduce_flashing && self.history_valid {
            // Map flashStrength (0..1) into smooth persistence (0..0.88) so motion stays responsive
            (settings.flash_strength.clamp(0.0, 1.0) * 0.88).clamp(0.0, 0.88)
        } else {
            0.0
        };
        let rgb = settings.tint_rgb();
        let u = PostUniforms {
            brightness: settings.brightness.clamp(0.05, 1.0),
            persistence,
            tint_enabled: if settings.tint_enabled { 1.0 } else { 0.0 },
            tint_strength: settings.tint_strength.clamp(0.0, 1.0),
            tint_r: rgb[0],
            tint_g: rgb[1],
            tint_b: rgb[2],
            _pad: 0.0,
        };
        queue.write_buffer(&self.uniform_buf, 0, bytemuck::bytes_of(&u));

        let read_idx = self.hist_idx;
        let write_idx = 1 - self.hist_idx;

        {
            let mut rp = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("dropkick-post-pass"),
                color_attachments: &[
                    Some(wgpu::RenderPassColorAttachment {
                        view: &self.display_view,
                        depth_slice: None,
                        resolve_target: None,
                        ops: wgpu::Operations {
                            load: wgpu::LoadOp::Clear(wgpu::Color::BLACK),
                            store: wgpu::StoreOp::Store,
                        },
                    }),
                    Some(wgpu::RenderPassColorAttachment {
                        view: &self.history_views[write_idx],
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
            rp.set_pipeline(&self.post_pipeline);
            rp.set_bind_group(0, &self.post_bgs[read_idx], &[]);
            rp.draw(0..3, 0..1);
        }

        self.hist_idx = write_idx;
        self.history_valid = settings.reduce_flashing;

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
            rp.set_bind_group(0, &self.blit_bg, &[]);
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
                rp.set_bind_group(0, &self.blit_bg, &[]);
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

#[allow(clippy::type_complexity)]
fn create_size_dependent(
    device: &wgpu::Device,
    width: u32,
    height: u32,
    sampler: &wgpu::Sampler,
    uniform_buf: &wgpu::Buffer,
    post_bgl: &wgpu::BindGroupLayout,
    blit_bgl: &wgpu::BindGroupLayout,
) -> (
    wgpu::Texture,
    wgpu::TextureView,
    wgpu::Texture,
    wgpu::TextureView,
    [wgpu::Texture; 2],
    [wgpu::TextureView; 2],
    [wgpu::BindGroup; 2],
    wgpu::BindGroup,
) {
    let scene_tex = create_rgba_tex(device, "dropkick-scene", width, height, false);
    let scene_view = scene_tex.create_view(&Default::default());
    let display_tex = create_rgba_tex(device, "dropkick-display", width, height, false);
    let display_view = display_tex.create_view(&Default::default());
    let h0 = create_rgba_tex(device, "dropkick-hist-0", width, height, false);
    let h1 = create_rgba_tex(device, "dropkick-hist-1", width, height, false);
    let hv0 = h0.create_view(&Default::default());
    let hv1 = h1.create_view(&Default::default());

    let make_post_bg = |prev: &wgpu::TextureView| {
        device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("dropkick-post-bg"),
            layout: post_bgl,
            entries: &[
                wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(&scene_view) },
                wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::TextureView(prev) },
                wgpu::BindGroupEntry { binding: 2, resource: wgpu::BindingResource::Sampler(sampler) },
                wgpu::BindGroupEntry { binding: 3, resource: uniform_buf.as_entire_binding() },
            ],
        })
    };
    let post_bgs = [make_post_bg(&hv0), make_post_bg(&hv1)];
    let blit_bg = device.create_bind_group(&wgpu::BindGroupDescriptor {
        label: Some("dropkick-blit-bg"),
        layout: blit_bgl,
        entries: &[
            wgpu::BindGroupEntry { binding: 0, resource: wgpu::BindingResource::TextureView(&display_view) },
            wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::Sampler(sampler) },
        ],
    });
    (scene_tex, scene_view, display_tex, display_view, [h0, h1], [hv0, hv1], post_bgs, blit_bg)
}
