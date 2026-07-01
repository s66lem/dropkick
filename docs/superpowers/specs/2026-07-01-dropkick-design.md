# Dropkick — Design Spec

**Date:** 2026-07-01
**Status:** Approved for planning
**Reference hardware:** Raspberry Pi 5, Raspberry Pi OS (aarch64), Mesa V3D driver (caps at OpenGL ES 3.1 / GLSL ES 3.10), HDMI output. Audio comes from any host capture device via PipeWire/SDL — the Elgato HD60 S+ is the author's source but Dropkick is **general-purpose**: the audio device is fully configurable and nothing is hardcoded to a specific capture card.

## 1. Overview

Dropkick is a reproducible fork-and-extend of [projectM](https://github.com/projectM-visualizer/projectm)
(libprojectM core) and [frontend-sdl-cpp](https://github.com/projectM-visualizer/frontend-sdl-cpp)
(the projectMSDL frontend), targeting a Raspberry Pi 5 audio visualizer with phone-based remote
control. It delivers:

1. **GLES 3.1 compatibility** via runtime capability detection (guarded, not hardcoded downgrades).
2. **Organized preset & texture management** with runtime pack switching.
3. **Phone remote control** over the LAN via an embedded HTTP server driving libprojectM's playlist API.
4. **Reproducible build + boot** via a bootstrap script and a systemd service.

Everything builds from source on the Pi (aarch64); no prebuilt x86 releases are used.

## 2. Architecture & repository layout

Dropkick is a thin **superproject** that pins two patched forks as git submodules and holds all glue.

```
dropkick/                         (NEW superproject repo → s66lem/dropkick)
├── external/
│   ├── projectm/                 (submodule → s66lem/dropkick-projectm, libprojectM fork)
│   └── frontend-sdl-cpp/         (submodule → s66lem/dropkick-frontend, projectMSDL fork)
├── patches/                      (exported .patch files, for documentation/audit/CI)
├── remote/                       (mobile web assets: index.html, app.js, style.css)
├── scripts/
│   ├── bootstrap.sh              (submodule init, apt deps, build, install)
│   └── build.sh                  (incremental rebuild)
├── config/
│   ├── dropkick.env              (audio source, preset root, texture dir — sourced by service)
│   └── projectMSDL.properties    (frontend config template)
├── systemd/
│   └── dropkick.service          (launches visualizer on boot to HDMI)
└── docs/                         (this spec, patch rationale, setup guide)
```

### Repo decisions (locked)

- **This repo** (currently `s66lem/dropkick`, a libprojectM fork with `upstream →
  projectM-visualizer/projectm`) is **renamed** to `s66lem/dropkick-projectm` and remains a pure
  libprojectM fork. This keeps `git pull upstream` clean — orchestration glue never mixes into
  libprojectM history.
- A **new** `s66lem/dropkick` superproject is created for the glue and submodules.
- A **new** `s66lem/dropkick-frontend` fork of `frontend-sdl-cpp` holds the frontend patches +
  embedded remote server.
- GLES/remote patches live as **real commits inside the forks** (so each builds standalone and can
  rebase on upstream). `patches/` holds exported `.patch` copies purely for documentation/audit.

## 3. GLES compatibility (guarded runtime detection)

The stock code assumes GLES 3.2; the Pi 5's V3D driver caps at 3.1 / GLSL ES 3.10. Rather than
hardcode a downgrade, Dropkick detects capability at runtime. The two changes are self-guarding: the
frontend requests the best context that works, and libprojectM accepts anything at or above 3.1.

### 3.1 Frontend — `src/SDLRenderingWindow.cpp` (fork: dropkick-frontend)

Replace the single hardcoded GLES context request with a **fallback ladder**: request
`SDL_GL_CONTEXT_PROFILE_ES` at 3.2, and if `SDL_GL_CreateContext` fails, retry at 3.1, then 3.0. The
first context that creates successfully wins.

- On the Pi's V3D driver, 3.2 fails and it lands on **3.1** — byte-for-byte the runtime behavior
  already validated by hand.
- On stronger GPUs it keeps 3.2 automatically.
- Log the version actually obtained.

### 3.2 libprojectM — `src/libprojectM/Renderer/Platform/GladLoader.cpp` (fork: dropkick-projectm)

Lower the GLES **minimum acceptance floor** in the `#ifdef USE_GLES` branch:

```cpp
// before
.WithMinimumVersion(3, 2)
.WithMinimumShaderLanguageVersion(3, 20)
// after
.WithMinimumVersion(3, 1)
.WithMinimumShaderLanguageVersion(3, 10)
```

This is a floor, not a request — it means "accept any context ≥ 3.1" so libprojectM stops rejecting
the 3.1 context the frontend just created. GLAD still loads whatever the driver actually exposes.

### 3.3 Build flag

Both forks are configured with `-DENABLE_GLES=TRUE`.

### 3.4 Fallback

If the runtime fallback ladder ever misbehaves, collapsing the frontend back to a single hardcoded
3.1 request is a one-line change. The `GladLoader` floor change is required in either case.

## 4. Preset & texture management

- **Config-time library root**, default `~/.local/share/dropkick/presets`, containing **pack
  subfolders**: `presets/cream-of-the-crop/`, `presets/my-picks/`, etc.
- **Texture root**, default `~/.local/share/dropkick/textures`.
- The frontend config (`projectMSDL.properties`) points at the active pack and the texture root.
- **Runtime pack switching:** the phone remote lists pack subfolders and, on selection, reloads the
  playlist from the chosen subfolder via the playlist API (`projectm_playlist_clear` +
  `projectm_playlist_add_path`).
- `bootstrap.sh` creates the directories and drops in a starter pack.
- Documented default locations and config keys in `docs/`.

## 5. Phone remote control (embedded HTTP server)

The remote lives **in-process** in the frontend, where the playlist and audio-device handles are.

### 5.1 Server

- Add **cpp-httplib** (single header) to the dropkick-frontend fork.
- On startup, bind `0.0.0.0:<port>` (default **8080**, configurable). Runs on its own thread; all
  calls that touch projectM/playlist state are marshalled to be thread-safe with the render loop.
- Optional shared-secret token (config key) required as a header/query param; bound to LAN only.

### 5.2 REST API

| Method & path         | Action                                                        |
|-----------------------|---------------------------------------------------------------|
| `GET  /`              | Serve the mobile web page                                     |
| `GET  /api/status`    | Current preset name, shuffle state, lock state, audio device  |
| `POST /api/next`      | Next preset (playlist API)                                     |
| `POST /api/prev`      | Previous preset                                                |
| `POST /api/random`    | Jump to a random preset                                        |
| `POST /api/preset`    | Jump to preset by index or name                               |
| `POST /api/shuffle`   | Toggle shuffle                                                 |
| `POST /api/lock`      | Toggle preset lock (lock/unlock)                              |
| `GET  /api/packs`     | List available pack subfolders                                |
| `POST /api/pack`      | Reload playlist from a named pack subfolder                    |
| `GET  /api/audio`     | List capture devices + current selection                      |
| `POST /api/audio/next`| Cycle to the next capture device (PipeWire/SDL device list)   |

### 5.3 Web page (`remote/`)

- Single mobile-first page: large touch buttons for next/prev/random, shuffle & lock toggles, a pack
  picker, and an audio-source cycler.
- Polls `GET /api/status` on an interval to reflect current preset/lock/shuffle/device.
- Served from the frontend's binary/asset path; reached at `http://<pi-ip>:8080`.

## 6. Reproducible build & boot

### 6.1 `scripts/bootstrap.sh`

1. `apt install` build deps: SDL2, GLES/EGL (`libgles-dev`, `libegl-dev`), PipeWire dev headers,
   `cmake`, `build-essential`, Poco (`libpoco-dev`), git.
2. `git submodule update --init --recursive`.
3. Configure + build **libprojectM** fork with `-DENABLE_GLES=TRUE`, install (staged prefix under
   `~/.local` or a Dropkick prefix).
4. Configure + build **frontend** fork against that libprojectM with `-DENABLE_GLES=TRUE`, install.
5. Create preset/texture dirs, install starter pack, install `projectMSDL.properties` from the
   template, install the systemd unit.
6. Idempotent and re-runnable.

### 6.2 `scripts/build.sh`

Incremental rebuild of both forks without re-running apt/dir setup.

### 6.3 `config/dropkick.env`

Sourced by the service; defines:
- `DROPKICK_AUDIO_SOURCE` — capture device to use (PipeWire node name or SDL device
  index/name). Generic: any capture device the host exposes. Unset = default/first capture device.
- `DROPKICK_PRESET_ROOT`, `DROPKICK_PRESET_PACK`, `DROPKICK_TEXTURE_ROOT`.
- `DROPKICK_REMOTE_PORT`, `DROPKICK_REMOTE_TOKEN`.

### 6.4 `systemd/dropkick.service`

- Launches projectMSDL fullscreen on HDMI at boot, on the graphical target.
- Sources `dropkick.env`, passes preset/texture/audio/port settings as args or via config.
- Restart-on-failure. Installed as a user service (or system service bound to the graphical session)
  — chosen during implementation based on how the Pi reaches the desktop/HDMI target.

## 7. Testing & verification

- **GLES patches:** build both forks with `-DENABLE_GLES=TRUE`; confirm on the Pi that the context
  lands on 3.1 and the visualizer renders. Confirm the fallback ladder logs the obtained version.
- **Remote API:** unit/smoke test each endpoint (curl) returns expected status and mutates state;
  verify from a phone browser on the LAN.
- **Preset packs:** verify pack switch reloads the playlist from the correct subfolder.
- **Boot:** reboot the Pi and confirm the service starts the visualizer on HDMI with the configured
  audio source, preset pack, and remote reachable.
- **Reproducibility:** run `bootstrap.sh` on a clean Pi OS image and confirm end-to-end success.

## 8. Out of scope (YAGNI)

- Full pack-manager UI (enable/disable, weighting, ratings persisted to disk).
- Cross-compilation / CI artifact builds (build-on-Pi only).
- Keypress-injection remote (rejected in favor of the API-driven server).
- Authentication beyond a single optional LAN shared-secret token.

## 9. Open items for the implementation plan

- Exact frontend integration points for playlist API, audio-device switching, and asset serving
  (to be confirmed by reading the `frontend-sdl-cpp` source during planning).
- Thread-safety mechanism for marshalling remote commands into the render loop.
- User vs. system systemd service, based on the Pi's HDMI/desktop session model.
