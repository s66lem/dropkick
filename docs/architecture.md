# Architecture

Dropkick is a fork-and-extend of [projectM](https://github.com/projectM-visualizer/projectm)
(`external/projectm`) and its SDL frontend (`external/frontend-sdl-cpp`, built as
`projectMSDL`). The frontend owns everything Dropkick adds: the render loop,
post-processing, the embedded HTTP remote, telemetry, and GPU-hang resilience.

```
external/frontend-sdl-cpp/src/
  RenderLoop         per-frame loop, input, watchdog, auto-skip
  ProjectMWrapper    libprojectM + playlist, blocklist, crash breadcrumb
  PostProcess        MRT + FBO ping-pong filter (persistence, brightness, tint)
  RemoteControl      embedded HTTP server, command queue, state snapshots
  SystemMonitor      CPU / memory / temperature sampling (Linux + Windows)
  PresetLists        favorites / dislikes / blocklist / slow-count persistence
  JsonUtil           JSON escaping / parsing helpers
  AudioCapture*      SDL and WASAPI capture backends
```

## Threading model

| Thread | Owns | Notes |
|---|---|---|
| **Render / GL** | GL context, libprojectM, playlist, `PostProcess`, input | The only thread that touches GL or libprojectM. Drains the remote command queue once per frame and publishes a compact status snapshot. |
| **Audio capture** | SDL / WASAPI callback | Pushes PCM into libprojectM's thread-safe audio buffer. Never touches GL. |
| **HTTP server** | cpp-httplib worker pool | Parses requests, validates input, enqueues `Command`s, and serializes JSON **lazily** from the latest snapshots. |
| **Monitor (1 Hz)** | `SystemMonitor`, workshop directory poll | Samples CPU/memory/temperature and watches the workshop folder for hot-reload, so neither runs on the render thread. |

Rules that keep this safe:

- **HTTP → render** is one-way through a mutex-protected command queue; HTTP
  handlers never call libprojectM or GL.
- **Render → HTTP** is by snapshot: the render thread copies small POD state
  (current preset, FPS, playlist revision) under a short lock once per frame. It
  never serializes JSON at 60 Hz; the HTTP thread formats the response when asked.
- Per-frame config (visual controls, auto-skip thresholds) is cached from
  property-change observers, so the hot loop does no string lookups or hex parsing.
- The playlist carries a **revision counter**; `/api/presets` uses it as an
  `ETag` and Browse stays in sync after dislikes or quarantines.
- Category-shuffle jumps use cached shuffle indices (`std::mt19937`), so a jump is O(1).

## GLES 3.1 detection and context fallback

The Raspberry Pi 5 (VideoCore VII, Mesa V3D) exposes OpenGL ES 3.1, while
upstream libprojectM requires 3.2. Rather than hardcode a downgrade, Dropkick
detects at runtime:

1. **Frontend** (`SDLRenderingWindow.cpp`) requests a GLES context in descending
   order: **3.2 → 3.1 → 3.0**, keeping the first that creates successfully. The Pi
   lands on 3.1; stronger GPUs keep 3.2.
2. **libprojectM** (`GladLoader.cpp`) accepts any context **≥ 3.1 / GLSL ES 3.10**
   (the acceptance floor was lowered from 3.2).
3. MilkDrop HLSL shaders cross-compile to GLSL ES 3.10 on these drivers.

On Windows and desktop Linux the frontend builds against desktop **OpenGL 3.3 Core**
(loaded with GLEW) instead. See [patches.md](patches.md).

## Post-processing pipeline

`PostProcess` implements *Reduce flashing*, *Brightness*, and *Screen tint* as a
single full-screen pass after projectM renders into an offscreen FBO.

```
projectM ──► scene FBO ──► post pass (MRT) ──┬─► output attachment ──► glBlitFramebuffer ──► backbuffer
                  ▲                          └─► history attachment ─┐
                  └──────────── ping-pong swap (previous ↔ current) ◄┘
```

- **Single pass, multiple render targets.** One fragment shader writes both the
  displayed frame and the next history frame, so the filter costs one draw.
- **Temporal persistence.** The shader blends the new frame with the previous
  (`flashStrength`, 0–1), limiting how fast brightness can rise between frames —
  this is what eases strobe-heavy presets.
- **FBO ping-pong.** Two history textures swap roles each frame; nothing is read
  back from the default framebuffer.
- **No readback stall.** The result is copied to the backbuffer with
  `glBlitFramebuffer`, never `glReadPixels`/`glCopyTexImage` from the backbuffer.
- **Brightness and monochrome tint** (`tintColor`, `tintStrength`) are applied in
  the same pass.
- **Targets:** GLES 3.0+ (`#version 300 es`; the Pi 5 runs it on 3.1) and desktop
  OpenGL 3.3 Core (`#version 330 core`, loaded with GLEW). When all effects are
  off the pass is bypassed.

## GPU-hang resilience

Some shader-heavy presets can hang the Pi's V3D GPU, sometimes taking the whole
board with it. Four layers, from cheapest to hardest:

| Layer | Detects | Action |
|---|---|---|
| **Frame watchdog** (`RenderLoop`) | Render + swap for one frame took longer than **4 s** | Checked when control returns to the render loop (it cannot interrupt a call that never returns). Quarantines the preset: skip and blocklist. |
| **Crash breadcrumb** (`ProjectMWrapper`) | Process died while a preset was active | `state/loading` is written in the preset-switched callback, so it names the preset that was rendering when a hang or crash happened after load. It is cleared on clean shutdown; if it exists at next start, that preset is quarantined. |
| **Low-FPS strikes** (`autoskip*`) | FPS below `autoskipFps` (default 20) for 1.5 s, after a 2 s per-preset grace period | Skips the preset; after `autoskipStrikes` (default 3) strikes, which persist across runs, it is blocklisted. Paused while `PostProcess` is active, so the post-pass cost isn't charged to presets. |
| **Hardware watchdog** (`scripts/enable-watchdog.sh`) | Whole-board lockup | The SoC watchdog reboots the Pi; the breadcrumb then quarantines the culprit. |

`scripts/seed-blocklist.sh` can pre-seed the blocklist with a static shader-size
heuristic; the app reads the blocklist at startup, so restart Dropkick to apply
it. Everything is reversible from the remote (Settings → Blocked presets → Clear).

### State files

All under `~/.local/share/dropkick/` (`%USERPROFILE%\.local\share\dropkick\` on Windows).

| File | Write mode |
|---|---|
| `favorites.json`, `slowcounts.txt` | Atomic: written to a `.tmp` file, then renamed over the target |
| `blocklist.txt`, `dislikes.txt` | Append one path per line |
| `state/loading` | Truncating write (single line), removed on clean shutdown |
| `workshop/_scratch.milk`, saved workshop presets | Atomic, via the same helper (`PresetLists::WriteFileAtomic`) |

## HTTP API

Served by the embedded cpp-httplib server (default port `8080`). The static web
app is served from `remote.webRoot`; with a token set, `GET /<token>` serves
`index.html` as a short entry URL, and `GET /<token>/` redirects (`302`) to
`/<token>`. The token is matched as a literal path (no regex route).

**Auth.** If `remote.token` is non-empty, every `/api/*` route requires it as the
`X-Dropkick-Token` header or `?token=` query parameter (constant-time compare);
otherwise it returns `401 {"error":"unauthorized"}`. The web app reads the token
from the URL path (`/op1`) and sends the header.

**Request bodies** are capped at **1 MiB**; larger requests get `413`.

| Method | Path | Params / body | Purpose |
|---|---|---|---|
| GET | `/api/status` | — | `preset`, `position`, `size`, `shuffle`, `locked`, `favorited`, `favoritesShuffle`, `categoryShuffle`, `workshop`, `blocked`, `disliked`, `fps`, `cpu`, `memUsed`, `memTotal`, `temp`, `hasTemp`, `audio`, plus `catalogRevision` (preset catalog revision, matches the `/api/presets` `ETag`) and `audioId` (id of the active capture device) |
| GET | `/api/presets` | — | Full library; `ETag` + `If-None-Match` → `304 Not Modified` |
| GET | `/api/packs` | — | Available preset packs |
| GET | `/api/settings` | — | Current settings |
| GET | `/api/favorites` | — | Favorite presets |
| GET | `/api/audio/devices` | — | `{"current": <id>, "devices": [{"id": <id>, "name": "…"}]}` |
| GET | `/api/workshop/source` | — | Source of the current preset |
| POST | `/api/next` · `/api/prev` · `/api/random` | — | Transport (prev is history-based) |
| POST | `/api/shuffle` · `/api/lock` | — | Toggle shuffle / preset lock |
| POST | `/api/preset` | `index` | Play preset by playlist index |
| POST | `/api/pack` | `name` | Switch pack |
| POST | `/api/settings` | `key`, `value` | Change a setting (validated and clamped) |
| POST | `/api/favorites/toggle` | `path` | Star / unstar a preset |
| POST | `/api/favorites/shuffle` | — | Toggle shuffle-favorites mode |
| POST | `/api/category/shuffle` | `path` | Shuffle within a category |
| POST | `/api/dislike` | — | Dislike the current preset and move on |
| POST | `/api/dislikes/clear` · `/api/blocklist/clear` | — | Clear dislikes / blocklist |
| POST | `/api/audio/select` | `index` = a device `id` from `/api/audio/devices` | Select a capture device (`400` for an unknown id) |
| POST | `/api/audio/next` | — | Cycle capture device |
| POST | `/api/workshop/capture` | — | Copy the current preset into `workshop/` |
| POST | `/api/workshop/apply` | body = preset text | Write `workshop/_scratch.milk` and load it on the display |
| POST | `/api/workshop/save` | `name`, optional `overwrite=0\|1`, body = preset text | Save into `workshop/`; with `overwrite=0` an existing file returns `409` |

Errors use `{"error":"…"}` with `400` (bad input), `401` (auth), `409` (save
would overwrite), `413` (body over 1 MiB), or `500`.
