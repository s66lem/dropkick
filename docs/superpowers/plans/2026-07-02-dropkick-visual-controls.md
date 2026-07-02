# Dropkick Visual Controls & Playback Quality — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a dislike button, a working strobe dampener, low-FPS autoskip, a brightness slider, and a monochrome screen tint — all driven from the phone remote.

**Architecture:** Brightness, monochrome tint, and the improved strobe reduction are all fullscreen post-process effects, so they share one GLES pass — the existing `StrobeFilter` (offscreen FBO render + fullscreen composite shader) is generalized into a `PostProcess` class. Dislike reuses the proven blocklist/quarantine machinery in `ProjectMWrapper` but with its own list. Low-FPS autoskip lives in `RenderLoop` (where FPS is known) backed by a persistent per-preset strike counter in `ProjectMWrapper`. All new settings flow through the existing `RemoteControl` settings API and the `remote/` web UI.

**Tech Stack:** C++17, GLES 3.1 (`#version 310 es` shaders), Poco (config/logging/subsystems), libprojectM 4 playlist API, cpp-httplib (vendored), SDL2, vanilla JS/HTML/CSS remote.

## Global Constraints

- **Builds and runs ONLY on the Raspberry Pi 5** (aarch64, Mesa V3D, GLES 3.1). The Windows dev host cannot compile or run this. There is **no local build and no unit-test harness** for this frontend — every task is verified on the Pi via `scripts/build.sh` plus curl/visual checks. Do not fabricate local test runs.
- **GLES-only post-processing.** All GL post-process code lives under `#if USE_GLES`; the `#else` desktop path is a no-op stub and `Active()` must return `false` there.
- **Shaders are `#version 310 es`** with `precision highp float;`.
- **Config key convention:** `RenderLoop` reads via `_userConfig` (a Poco view rooted at `projectM`, so `getBool("tintEnabled")` reads `projectM.tintEnabled`). `RemoteControl::ApplySetting` writes the **full** key `app.UserConfiguration()->setX("projectM.tintEnabled", …)`. The settings snapshot in `PublishStatus` reads the full key from `…instance().config()`. Follow this exactly — it is how `reduceFlashing`/`flashStrength`/`fps` already work.
- **Render-thread rule:** HTTP handlers only enqueue commands and read mutex-guarded snapshots; all projectM/playlist/config mutation happens on the render thread in `DrainCommands()`.
- **Commit trailer:** end every commit message with:
  ```
  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01PGfsm6gg5JXx1LSotdT9k9
  ```

## Repository & build facts (read before starting)

- Working repo: `C:\Users\s6lem\Documents\GitHub\dropkick-superproject` (remote `s66lem/dropkick`, branch `main`). **Not** the sibling `dropkick` folder (that's the old libprojectM-only fork).
- Frontend sources: `external/frontend-sdl-cpp/src/`.
- On the Pi, incremental build + install: `./scripts/build.sh`.
- Remote web assets are served from `~/.local/share/dropkick/remote/`. After editing `remote/*`, deploy with:
  `cp -r remote/. "$HOME/.local/share/dropkick/remote/"`
- Runtime config file the app reads: `$PREFIX/projectMSDL.properties`, rendered by `./scripts/sync-config.sh` from `config/projectMSDL.properties.in`. New static `projectM.*` defaults go in `config/projectMSDL.properties.in` (Task 5).
- Reliable relaunch on the Pi (from memory): the user launches from the Sound & Video menu, or:
  `systemd-run --user --unit=dk -E WAYLAND_DISPLAY=wayland-0 -E DISPLAY=:0 ~/.local/bin/dropkick`
- Remote base URL: `http://sleempie.local:8080` (append `?token=…` / header `X-Dropkick-Token` only if a token is configured; default is empty).

---

## Task 1: PostProcess pass (strobe fix + brightness + tint plumbing)

Generalize `StrobeFilter` into `PostProcess`: one offscreen FBO render and one fullscreen composite shader that does temporal persistence (the strobe fix), brightness, and monochrome tint. Wire `RenderLoop` to feed it config values each frame and bypass it entirely when no effect is active. Brightness and tint have no remote UI yet (Task 2) but are testable via temporary config keys.

**Files:**
- Rename: `external/frontend-sdl-cpp/src/StrobeFilter.h` → `PostProcess.h`
- Rename: `external/frontend-sdl-cpp/src/StrobeFilter.cpp` → `PostProcess.cpp`
- Modify: `external/frontend-sdl-cpp/src/CMakeLists.txt:23-24` (source list)
- Modify: `external/frontend-sdl-cpp/src/RenderLoop.h:7,100` (include + member)
- Modify: `external/frontend-sdl-cpp/src/RenderLoop.cpp:50-63` (per-frame wiring)

**Interfaces:**
- Produces (`PostProcess`):
  - `void SetReduceFlashing(bool enabled)`
  - `void SetStrength(float strength)` — 0..1
  - `void SetBrightness(float brightness)` — 0..1
  - `void SetTint(bool enabled, const std::string& hexColor, float strength)` — hex like `00ff00` or `#00ff00`, strength 0..1
  - `bool Active() const` — true if any effect is on (GLES); always false on desktop
  - `void Begin(int width, int height)` — binds offscreen FBO; no-op if inactive
  - `uint32_t SceneFbo() const` — FBO to render projectM into (0 if setup failed → backbuffer)
  - `void Composite()` — draws the processed scene to the backbuffer
- Consumes: `ProjectMWrapper::RenderFrame(uint32_t targetFbo)` (already exists).

- [ ] **Step 1: Rename the files with git**

```bash
cd external/frontend-sdl-cpp/src
git mv StrobeFilter.h PostProcess.h
git mv StrobeFilter.cpp PostProcess.cpp
cd -
```

- [ ] **Step 2: Replace `PostProcess.h` with the generalized interface**

Overwrite `external/frontend-sdl-cpp/src/PostProcess.h` with:

```cpp
#pragma once

#include <cstdint>
#include <string>

/**
 * @brief Fullscreen post-process pass (GLES 3.1 only).
 *
 * projectM renders into an offscreen framebuffer; a single fullscreen shader then
 * applies, per pixel:
 *   1. Temporal persistence  — blend toward the previous displayed frame (strobe/flash reduction).
 *   2. Brightness            — uniform multiply.
 *   3. Monochrome tint       — collapse to luminance, remap to a single hue, mix by intensity.
 * The displayed frame is copied back into the "previous" texture for next frame's persistence.
 *
 * The pass runs only when Active() is true (any effect enabled); otherwise the caller
 * renders straight to the backbuffer. On non-GLES builds every method is a no-op and
 * Active() is false.
 */
class PostProcess
{
public:
    ~PostProcess();

    void SetReduceFlashing(bool enabled);
    void SetStrength(float strength);      //!< 0..1, higher = more persistence/smoothing.
    void SetBrightness(float brightness);  //!< 0..1 screen brightness multiplier.
    /** @param hexColor "#rrggbb" or "rrggbb". @param strength 0=original colors, 1=full monochrome. */
    void SetTint(bool enabled, const std::string& hexColor, float strength);

    /** @return true if any effect is active (persistence, dimming, or tint). */
    bool Active() const;

    /** Binds the offscreen scene framebuffer at the given size. No-op if inactive. */
    void Begin(int width, int height);

    /** @return framebuffer id to render the scene into (valid after Begin()); 0 if setup failed. */
    uint32_t SceneFbo() const;

    /** Draws the processed scene to the backbuffer and stores it for next frame. No-op if inactive. */
    void Composite();

private:
    void EnsureResources(int width, int height);
    void Destroy();
    static bool ParseHexColor(const std::string& hex, float& r, float& g, float& b);

    bool _reduceFlashing{false};
    float _strength{0.6f};
    float _brightness{1.0f};
    bool _tintEnabled{false};
    float _tintR{0.0f};
    float _tintG{1.0f};
    float _tintB{0.0f};
    float _tintStrength{1.0f};

    int _width{0};
    int _height{0};

    unsigned int _fbo{0};
    unsigned int _sceneTex{0};
    unsigned int _depthRb{0};
    unsigned int _prevTex{0};
    unsigned int _vao{0};
    unsigned int _program{0};
    int _uPersistence{-1};
    int _uBrightness{-1};
    int _uTintEnabled{-1};
    int _uTintColor{-1};
    int _uTintStrength{-1};
    bool _ready{false};
};
```

- [ ] **Step 3: Replace `PostProcess.cpp` with the generalized implementation**

Overwrite `external/frontend-sdl-cpp/src/PostProcess.cpp` with:

```cpp
#include "PostProcess.h"

#if USE_GLES

#include <GLES3/gl3.h>

#include <Poco/Logger.h>

#include <algorithm>
#include <string>

namespace
{
Poco::Logger& logger() { return Poco::Logger::get("PostProcess"); }

const char* kVertexShader = R"(#version 310 es
out vec2 vUV;
void main()
{
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUV = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* kFragmentShader = R"(#version 310 es
precision highp float;
uniform sampler2D uScene;
uniform sampler2D uPrev;
uniform float uPersistence;   // 0 = no smoothing, ->1 = heavy trails
uniform float uBrightness;    // screen multiplier
uniform float uTintEnabled;   // >0.5 = tint on
uniform vec3  uTintColor;     // target hue
uniform float uTintStrength;  // 0 = original, 1 = full monochrome
in vec2 vUV;
out vec4 frag;
void main()
{
    vec3 scene    = texture(uScene, vUV).rgb;
    vec3 prev     = texture(uPrev,  vUV).rgb;
    vec3 smoothed = mix(scene, prev, uPersistence);   // temporal low-pass: dampens flashes + hue cycling
    vec3 c        = smoothed * uBrightness;
    if (uTintEnabled > 0.5)
    {
        float lum  = dot(c, vec3(0.2126, 0.7152, 0.0722));
        vec3  mono = lum * uTintColor;
        c = mix(c, mono, uTintStrength);
    }
    frag = vec4(c, 1.0);
}
)";

GLuint CompileShader(GLenum type, const char* src)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        poco_error_f1(logger(), "Post-process shader compile failed: %s", std::string(log));
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}
} // namespace

PostProcess::~PostProcess()
{
    Destroy();
}

bool PostProcess::ParseHexColor(const std::string& hex, float& r, float& g, float& b)
{
    std::string h = hex;
    if (!h.empty() && h[0] == '#') { h = h.substr(1); }
    if (h.size() != 6) { return false; }
    for (char c : h)
    {
        if (!std::isxdigit(static_cast<unsigned char>(c))) { return false; }
    }
    auto hexByte = [](const std::string& s) {
        return static_cast<float>(std::stoi(s, nullptr, 16)) / 255.0f;
    };
    r = hexByte(h.substr(0, 2));
    g = hexByte(h.substr(2, 2));
    b = hexByte(h.substr(4, 2));
    return true;
}

void PostProcess::SetReduceFlashing(bool enabled) { _reduceFlashing = enabled; }

void PostProcess::SetStrength(float strength)
{
    _strength = std::min(1.0f, std::max(0.0f, strength));
}

void PostProcess::SetBrightness(float brightness)
{
    _brightness = std::min(1.0f, std::max(0.0f, brightness));
}

void PostProcess::SetTint(bool enabled, const std::string& hexColor, float strength)
{
    _tintEnabled = enabled;
    _tintStrength = std::min(1.0f, std::max(0.0f, strength));
    float r, g, b;
    if (ParseHexColor(hexColor, r, g, b)) { _tintR = r; _tintG = g; _tintB = b; }
}

bool PostProcess::Active() const
{
    return _reduceFlashing || _brightness < 0.999f || _tintEnabled;
}

uint32_t PostProcess::SceneFbo() const
{
    return _fbo;
}

void PostProcess::EnsureResources(int width, int height)
{
    if (_ready && width == _width && height == _height)
    {
        return;
    }
    Destroy();
    _width = width;
    _height = height;

    glGenTextures(1, &_sceneTex);
    glBindTexture(GL_TEXTURE_2D, _sceneTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &_prevTex);
    glBindTexture(GL_TEXTURE_2D, _prevTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenRenderbuffers(1, &_depthRb);
    glBindRenderbuffer(GL_RENDERBUFFER, _depthRb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);

    glGenFramebuffers(1, &_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, _fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _sceneTex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, _depthRb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        poco_error(logger(), "Post-process framebuffer incomplete; disabling.");
        Destroy();
        _reduceFlashing = false;
        _brightness = 1.0f;
        _tintEnabled = false;
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (_program == 0)
    {
        GLuint vs = CompileShader(GL_VERTEX_SHADER, kVertexShader);
        GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFragmentShader);
        if (vs && fs)
        {
            _program = glCreateProgram();
            glAttachShader(_program, vs);
            glAttachShader(_program, fs);
            glLinkProgram(_program);
            GLint ok = GL_FALSE;
            glGetProgramiv(_program, GL_LINK_STATUS, &ok);
            if (!ok)
            {
                poco_error(logger(), "Post-process program link failed; disabling.");
                glDeleteProgram(_program);
                _program = 0;
            }
            else
            {
                glUseProgram(_program);
                glUniform1i(glGetUniformLocation(_program, "uScene"), 0);
                glUniform1i(glGetUniformLocation(_program, "uPrev"), 1);
                _uPersistence = glGetUniformLocation(_program, "uPersistence");
                _uBrightness = glGetUniformLocation(_program, "uBrightness");
                _uTintEnabled = glGetUniformLocation(_program, "uTintEnabled");
                _uTintColor = glGetUniformLocation(_program, "uTintColor");
                _uTintStrength = glGetUniformLocation(_program, "uTintStrength");
                glUseProgram(0);
            }
        }
        if (vs) { glDeleteShader(vs); }
        if (fs) { glDeleteShader(fs); }
        if (_program == 0)
        {
            Destroy();
            _reduceFlashing = false;
            _brightness = 1.0f;
            _tintEnabled = false;
            return;
        }
    }

    if (_vao == 0)
    {
        glGenVertexArrays(1, &_vao);
    }

    _ready = true;
}

void PostProcess::Begin(int width, int height)
{
    if (!Active() || width <= 0 || height <= 0)
    {
        return;
    }
    EnsureResources(width, height);
    if (!_ready)
    {
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, _fbo);
    glViewport(0, 0, _width, _height);
}

void PostProcess::Composite()
{
    if (!Active() || !_ready)
    {
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, _width, _height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    glUseProgram(_program);
    // Persistence only applies when "reduce flashing" is on. k in [0, 0.9].
    float persistence = _reduceFlashing ? (0.9f * _strength) : 0.0f;
    glUniform1f(_uPersistence, persistence);
    glUniform1f(_uBrightness, _brightness);
    glUniform1f(_uTintEnabled, _tintEnabled ? 1.0f : 0.0f);
    glUniform3f(_uTintColor, _tintR, _tintG, _tintB);
    glUniform1f(_uTintStrength, _tintStrength);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, _sceneTex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, _prevTex);

    glBindVertexArray(_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    // Store the displayed frame as "previous" for next frame's persistence term.
    // (Persistence is computed in displayed space; at the default brightness=1 / tint-off this is
    // an exact EMA of the scene. Tint preserves luminance so tinted trails are correct; dimming
    // only darkens trails slightly — cosmetically fine.)
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, _prevTex);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, _width, _height);
    glBindTexture(GL_TEXTURE_2D, 0);

    glUseProgram(0);
}

void PostProcess::Destroy()
{
    if (_fbo) { glDeleteFramebuffers(1, &_fbo); _fbo = 0; }
    if (_sceneTex) { glDeleteTextures(1, &_sceneTex); _sceneTex = 0; }
    if (_prevTex) { glDeleteTextures(1, &_prevTex); _prevTex = 0; }
    if (_depthRb) { glDeleteRenderbuffers(1, &_depthRb); _depthRb = 0; }
    if (_vao) { glDeleteVertexArrays(1, &_vao); _vao = 0; }
    if (_program) { glDeleteProgram(_program); _program = 0; }
    _ready = false;
    _width = 0;
    _height = 0;
}

#else // !USE_GLES — no-op stub for desktop builds

PostProcess::~PostProcess() {}
bool PostProcess::ParseHexColor(const std::string&, float&, float&, float&) { return false; }
void PostProcess::SetReduceFlashing(bool) {}
void PostProcess::SetStrength(float) {}
void PostProcess::SetBrightness(float) {}
void PostProcess::SetTint(bool, const std::string&, float) {}
bool PostProcess::Active() const { return false; }
void PostProcess::Begin(int, int) {}
uint32_t PostProcess::SceneFbo() const { return 0; }
void PostProcess::Composite() {}
void PostProcess::EnsureResources(int, int) {}
void PostProcess::Destroy() {}

#endif
```

Note: the desktop stub references `_program` etc. via `Destroy()` never being GL-called — the members still exist (declared in the header, no `#if` around them), so the stub compiles. `<cctype>` for `std::isxdigit` is pulled in transitively via `<string>`/`<algorithm>` on the Pi toolchain; if the build errors on `isxdigit`, add `#include <cctype>` under the GLES branch includes.

- [ ] **Step 4: Update the CMake source list**

In `external/frontend-sdl-cpp/src/CMakeLists.txt`, replace lines 23-24:

```cmake
        StrobeFilter.cpp
        StrobeFilter.h
```

with:

```cmake
        PostProcess.cpp
        PostProcess.h
```

- [ ] **Step 5: Update RenderLoop.h include and member**

In `external/frontend-sdl-cpp/src/RenderLoop.h`:

Replace line 7:
```cpp
#include "StrobeFilter.h"
```
with:
```cpp
#include "PostProcess.h"
```

Replace line 100:
```cpp
    StrobeFilter _strobe; //!< Optional "reduce flashing" post-process (off unless enabled in settings).
```
with:
```cpp
    PostProcess _post; //!< Fullscreen post-process (persistence/brightness/tint); inert unless an effect is on.
```

- [ ] **Step 6: Wire RenderLoop to the generalized pass**

In `external/frontend-sdl-cpp/src/RenderLoop.cpp`, replace the block at lines 50-63 (from `_strobe.SetEnabled(...)` through the `else { _projectMWrapper.RenderFrame(); }`) with:

```cpp
        _post.SetReduceFlashing(_userConfig->getBool("reduceFlashing", false));
        _post.SetStrength(static_cast<float>(_userConfig->getDouble("flashStrength", 0.6)));
        _post.SetBrightness(static_cast<float>(_userConfig->getDouble("brightness", 1.0)));
        _post.SetTint(_userConfig->getBool("tintEnabled", false),
                      _userConfig->getString("tintColor", "#00ff00"),
                      static_cast<float>(_userConfig->getDouble("tintStrength", 1.0)));

        Uint32 renderStart = SDL_GetTicks();
        if (_post.Active())
        {
            _post.Begin(_renderWidth, _renderHeight);
            _projectMWrapper.RenderFrame(_post.SceneFbo()); // SceneFbo()==0 if setup failed -> backbuffer
            _post.Composite();
        }
        else
        {
            _projectMWrapper.RenderFrame();
        }
```

(The surrounding lines — the `Uint32 renderStart` watchdog check at old lines 69-73 — are unchanged; note `renderStart` is now declared inside this block, exactly as before.)

- [ ] **Step 7: Build on the Pi**

```bash
./scripts/build.sh
```
Expected: `Build complete. Binary: <prefix>/bin/projectMSDL` with no compile/link errors. If `isxdigit` errors, add `#include <cctype>` to `PostProcess.cpp` (GLES branch) and rebuild.

- [ ] **Step 8: Verify on the Pi (visual + smoke)**

Launch Dropkick. Then:
- **Strobe:** in the remote Settings, turn **Reduce flashing** ON, strength ~0.6, and watch a fast/strobing preset. Expected: visibly smoothed/trailing motion (much stronger than before). Turn OFF → back to crisp.
- **Brightness (temporary, no UI yet):** stop the app, add `projectM.brightness = 0.4` to `$PREFIX/projectMSDL.properties`, relaunch. Expected: screen noticeably dimmer. Remove the line afterward.
- **Tint (temporary):** add `projectM.tintEnabled = true` and `projectM.tintColor = #00ff00`, relaunch. Expected: screen is green monochrome. Remove afterward.
- Confirm GPU reset counter stays at 0 (no crash): `cat /sys/kernel/debug/dri/*/v3d_gpu_reset 2>/dev/null || true` (best-effort; mainly confirm the app didn't crash).

- [ ] **Step 9: Commit**

```bash
git add external/frontend-sdl-cpp/src/PostProcess.cpp external/frontend-sdl-cpp/src/PostProcess.h \
        external/frontend-sdl-cpp/src/CMakeLists.txt \
        external/frontend-sdl-cpp/src/RenderLoop.h external/frontend-sdl-cpp/src/RenderLoop.cpp
git commit -m "$(cat <<'EOF'
feat(render): unify post-processing into PostProcess (persistence strobe fix + brightness + tint)

Replaces the per-channel strobe clamp with a temporal persistence blend and
adds brightness/monochrome-tint uniforms to the same fullscreen pass. RenderLoop
bypasses the pass entirely when no effect is active.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01PGfsm6gg5JXx1LSotdT9k9
EOF
)"
```

---

## Task 2: Remote controls for brightness & monochrome tint

Expose brightness and tint through the settings API and the web UI.

**Files:**
- Modify: `external/frontend-sdl-cpp/src/RemoteControl.cpp` — `kKeys` set (~line 209), `ApplySetting` (~line 484-499), settings snapshot in `PublishStatus` (~line 669-680)
- Modify: `remote/index.html` — new Settings sections
- Modify: `remote/app.js` — load/handle new controls

**Interfaces:**
- Consumes: `PostProcess::SetBrightness`, `PostProcess::SetTint` (Task 1) via config keys `projectM.brightness`, `projectM.tintEnabled`, `projectM.tintColor`, `projectM.tintStrength`.
- Produces: settings API keys `brightness`, `tintEnabled`, `tintColor`, `tintStrength`; settings-snapshot JSON fields of the same names.

- [ ] **Step 1: Extend the settings allow-set**

In `external/frontend-sdl-cpp/src/RemoteControl.cpp`, in the `POST /api/settings` handler, replace the `kKeys` set (currently lines ~209-212):

```cpp
        static const std::set<std::string> kKeys{
            "presetDuration", "softCutDuration", "hardCut", "hardCutDuration",
            "hardCutSensitivity", "beatSensitivity", "fps", "aspectCorrection",
            "reduceFlashing", "flashStrength"};
```

with:

```cpp
        static const std::set<std::string> kKeys{
            "presetDuration", "softCutDuration", "hardCut", "hardCutDuration",
            "hardCutSensitivity", "beatSensitivity", "fps", "aspectCorrection",
            "reduceFlashing", "flashStrength",
            "brightness", "tintEnabled", "tintColor", "tintStrength"};
```

- [ ] **Step 2: Handle the new keys in ApplySetting**

In `ApplySetting` (after the `flashStrength` branch, ~line 499), add before the closing brace of the function:

```cpp
    else if (key == "brightness") { app.UserConfiguration()->setDouble("projectM.brightness", v); }
    else if (key == "tintEnabled") { app.UserConfiguration()->setBool("projectM.tintEnabled", on); }
    else if (key == "tintColor") { app.UserConfiguration()->setString("projectM.tintColor", value); }
    else if (key == "tintStrength") { app.UserConfiguration()->setDouble("projectM.tintStrength", v); }
```

- [ ] **Step 3: Report the new keys in the settings snapshot**

In `PublishStatus`, inside the `if (pm)` settings block, add these lines before the closing `<< "}";` (i.e. after the `flashStrength` line ~679 — add a trailing comma to that line):

Change:
```cpp
                 << "\"flashStrength\":" << ProjectMSDLApplication::instance().config().getDouble("projectM.flashStrength", 0.6)
                 << "}";
```
to:
```cpp
                 << "\"flashStrength\":" << ProjectMSDLApplication::instance().config().getDouble("projectM.flashStrength", 0.6) << ","
                 << "\"brightness\":" << ProjectMSDLApplication::instance().config().getDouble("projectM.brightness", 1.0) << ","
                 << "\"tintEnabled\":" << (ProjectMSDLApplication::instance().config().getBool("projectM.tintEnabled", false) ? "true" : "false") << ","
                 << "\"tintColor\":\"" << JsonEscape(ProjectMSDLApplication::instance().config().getString("projectM.tintColor", "#00ff00")) << "\","
                 << "\"tintStrength\":" << ProjectMSDLApplication::instance().config().getDouble("projectM.tintStrength", 1.0)
                 << "}";
```

- [ ] **Step 4: Add the Settings UI**

In `remote/index.html`, immediately after the "Reduce flashing" `<div class="set">…</div>` block (which ends at line 101, the `</div>` after `id="sFlash"`), insert:

```html
    <div class="set"><div class="h"><b>Brightness</b><span class="readout"><span id="vBright">—</span>%</span></div>
      <div class="desc">Dim the whole screen.</div>
      <input type="range" id="sBright" data-key="brightness" min="0.1" max="1" step="0.05"></div>
    <div class="set">
      <div class="tg"><b>Screen tint</b><div class="sw" id="tTint"></div></div>
      <div class="desc">Recolor everything to a single hue (true monochrome).</div>
      <div class="h mt"><span class="label">Color</span><input type="color" id="tintColor" value="#00ff00" class="colorpick"></div>
      <div class="swatches" id="tintSwatches">
        <button class="swatch" data-color="#00ff00" style="background:#00ff00"></button>
        <button class="swatch" data-color="#0080ff" style="background:#0080ff"></button>
        <button class="swatch" data-color="#ff40c0" style="background:#ff40c0"></button>
        <button class="swatch" data-color="#ffb000" style="background:#ffb000"></button>
        <button class="swatch" data-color="#00e5e5" style="background:#00e5e5"></button>
        <button class="swatch" data-color="#ff3030" style="background:#ff3030"></button>
        <button class="swatch" data-color="#ffffff" style="background:#ffffff"></button>
      </div>
      <div class="h mt"><span class="label">Intensity</span><span class="readout"><span id="vTintS">—</span>%</span></div>
      <input type="range" id="sTintS" data-key="tintStrength" min="0" max="1" step="0.05">
    </div>
```

- [ ] **Step 5: Add matching styles**

In `remote/style.css`, append:

```css
.colorpick { width: 46px; height: 28px; border: none; background: none; padding: 0; }
.swatches { display: flex; gap: 8px; margin: 8px 0 2px; flex-wrap: wrap; }
.swatch { width: 30px; height: 30px; border-radius: 6px; border: 2px solid rgba(255,255,255,.25); cursor: pointer; }
.swatch.on { border-color: #fff; }
```

- [ ] **Step 6: Wire the controls in app.js**

In `remote/app.js`, in `loadSettings()` (after the `tFlash` line ~207), add:

```javascript
    $("sBright").value = s.brightness; $("vBright").textContent = Math.round(s.brightness * 100);
    $("tTint").classList.toggle("on", !!s.tintEnabled);
    $("tintColor").value = s.tintColor || "#00ff00";
    $("sTintS").value = s.tintStrength; $("vTintS").textContent = Math.round(s.tintStrength * 100);
```

Update the `valueLabel` map (line ~210) to include the two new percent sliders and give them a percent formatter. Replace the `valueLabel` block and its loop (lines ~210-214) with:

```javascript
const valueLabel = { sDur: "vDur", sSoft: "vSoft", sBeat: "vBeat", sHardS: "vHardS", sHardD: "vHardD", sFps: "vFps", sFlash: "vFlash" };
for (const [sid, vid] of Object.entries(valueLabel)) {
  $(sid).addEventListener("input", () => { $(vid).textContent = fmt1(parseFloat($(sid).value)); });
  $(sid).addEventListener("change", () => POST(`/api/settings?key=${$(sid).dataset.key}&value=${$(sid).value}`));
}
// Percent-labelled sliders (0..1 shown as 0..100%).
const pctLabel = { sBright: "vBright", sTintS: "vTintS" };
for (const [sid, vid] of Object.entries(pctLabel)) {
  $(sid).addEventListener("input", () => { $(vid).textContent = Math.round(parseFloat($(sid).value) * 100); });
  $(sid).addEventListener("change", () => POST(`/api/settings?key=${$(sid).dataset.key}&value=${$(sid).value}`));
}
$("tTint").onclick = () => {
  const on = !$("tTint").classList.contains("on");
  $("tTint").classList.toggle("on", on);
  POST(`/api/settings?key=tintEnabled&value=${on}`);
};
$("tintColor").addEventListener("change", () => {
  POST(`/api/settings?key=tintColor&value=${encodeURIComponent($("tintColor").value.replace("#", ""))}`);
});
document.querySelectorAll("#tintSwatches .swatch").forEach(sw => sw.onclick = () => {
  const col = sw.dataset.color;
  $("tintColor").value = col;
  $("tTint").classList.add("on");
  POST(`/api/settings?key=tintColor&value=${encodeURIComponent(col.replace("#", ""))}`);
  POST(`/api/settings?key=tintEnabled&value=true`);
});
```

Note the color value is sent **without** the leading `#` (so it isn't parsed as a URL fragment); `PostProcess::ParseHexColor` accepts both forms.

- [ ] **Step 7: Build, deploy web assets, and verify on the Pi**

```bash
./scripts/build.sh
cp -r remote/. "$HOME/.local/share/dropkick/remote/"
```
Relaunch Dropkick, then verify the API round-trips:
```bash
curl -s -X POST "http://sleempie.local:8080/api/settings?key=brightness&value=0.4"
curl -s "http://sleempie.local:8080/api/settings" | tr ',' '\n' | grep -E 'brightness|tint'
```
Expected: `{"ok":true}` then the snapshot shows `"brightness":0.4` and the tint fields. Set brightness back to 1. On the remote's Settings tab: drag Brightness → screen dims; toggle Screen tint + tap a swatch → screen becomes that hue; drag Intensity → color washes in/out. Confirm the color picker updates the screen.

- [ ] **Step 8: Commit**

```bash
git add external/frontend-sdl-cpp/src/RemoteControl.cpp remote/index.html remote/app.js remote/style.css
git commit -m "$(cat <<'EOF'
feat(remote): brightness slider and monochrome screen tint controls

Adds brightness/tintEnabled/tintColor/tintStrength to the settings API and a
Settings UI (slider, color picker, quick swatches, intensity).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01PGfsm6gg5JXx1LSotdT9k9
EOF
)"
```

---

## Task 3: Low-FPS autoskip with strike counter

Skip presets that render below a threshold FPS; after N strikes a preset is blocklisted so it stops returning.

**Files:**
- Modify: `external/frontend-sdl-cpp/src/ProjectMWrapper.h` — add `<map>` include, public `AutoSkipSlow`, private slow-count members/methods
- Modify: `external/frontend-sdl-cpp/src/ProjectMWrapper.cpp` — init paths + load, `AutoSkipSlow`, `LoadSlowCounts`/`SaveSlowCounts`, includes
- Modify: `external/frontend-sdl-cpp/src/RenderLoop.h` — autoskip timing state
- Modify: `external/frontend-sdl-cpp/src/RenderLoop.cpp` — capture status once, preset-change detection, autoskip check
- Modify: `external/frontend-sdl-cpp/src/RemoteControl.cpp` — settings keys + snapshot
- Modify: `remote/index.html`, `remote/app.js` — Settings UI

**Interfaces:**
- Produces (`ProjectMWrapper`): `void AutoSkipSlow(uint32_t strikesThreshold)` — increments the current preset's slow-strike count (persisted); at/over the threshold, blocklists + removes it; otherwise advances to the next preset.
- Consumes: existing `AddToBlocklist`, `ApplyRemovalLists` (Task 4 renames `ApplyBlocklist`; if Task 3 lands first, call `ApplyBlocklist` here and let Task 4 rename it — see note).
- Settings API keys: `autoskipEnabled`, `autoskipFps`, `autoskipStrikes`.

> **Ordering note:** This plan lists Task 3 before Task 4, but Task 4 renames `ApplyBlocklist`→`ApplyRemovalLists`. To avoid a dangling reference, in Step 3 below call **`ApplyBlocklist()`** (its current name); Task 4's rename step will sweep this call site too. If you execute Task 4 first, call `ApplyRemovalLists()` instead.

- [ ] **Step 1: Declare the strike-counter API in the header**

In `external/frontend-sdl-cpp/src/ProjectMWrapper.h`:

Add `#include <map>` alongside the other includes (after `#include <cstdint>`).

After the `void ClearBlocklist();` declaration (line 136), add:

```cpp
    /**
     * @brief Records that the current preset ran too slowly and skips it. Increments a persistent
     * per-preset strike count; once it reaches @p strikesThreshold the preset is blocklisted and
     * removed. Below the threshold it is only skipped and can recover on a later, faster run.
     */
    void AutoSkipSlow(uint32_t strikesThreshold);
```

In the private section, after `void QuarantineFromCrash();` (line 169), add:

```cpp
    void LoadSlowCounts();                          //!< Read persistent low-FPS strike counts.
    void SaveSlowCounts();                          //!< Persist strike counts to disk.
```

After the `_breadcrumbPath` member (line 173), add:

```cpp
    std::map<std::string, uint32_t> _slowCounts; //!< Preset path -> low-FPS auto-skip strikes.
    std::string _slowCountsPath;                 //!< ~/.local/share/dropkick/slowcounts.txt
```

- [ ] **Step 2: Initialize paths and load counts**

In `external/frontend-sdl-cpp/src/ProjectMWrapper.cpp`, ensure `<cstdlib>` (for `strtoul`) and `<map>` are available — they are pulled in via existing includes/`ProjectMWrapper.h`; if `strtoul` errors, add `#include <cstdlib>`.

In `initialize`, after the `QuarantineFromCrash();` line (line 37), add:

```cpp
    _slowCountsPath = Poco::Path::expand("~/.local/share/dropkick/slowcounts.txt");
    LoadSlowCounts();
```

- [ ] **Step 3: Implement AutoSkipSlow and the load/save helpers**

In `external/frontend-sdl-cpp/src/ProjectMWrapper.cpp`, after `ClearBlocklist()` (ends line 411), add:

```cpp
void ProjectMWrapper::LoadSlowCounts()
{
    _slowCounts.clear();
    std::ifstream in(_slowCountsPath);
    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        auto tab = line.find('\t');
        if (tab == std::string::npos) { continue; }
        uint32_t count = static_cast<uint32_t>(std::strtoul(line.substr(0, tab).c_str(), nullptr, 10));
        std::string path = line.substr(tab + 1);
        if (!path.empty() && count > 0) { _slowCounts[path] = count; }
    }
}

void ProjectMWrapper::SaveSlowCounts()
{
    std::ofstream out(_slowCountsPath, std::ios::trunc);
    if (!out) { return; }
    for (const auto& kv : _slowCounts) { out << kv.second << '\t' << kv.first << "\n"; }
}

void ProjectMWrapper::AutoSkipSlow(uint32_t strikesThreshold)
{
    if (!_playlist) { return; }
    uint32_t pos = projectm_playlist_get_position(_playlist);
    char* item = projectm_playlist_item(_playlist, pos);
    std::string path = item ? item : "";
    if (item) { projectm_playlist_free_string(item); }
    if (path.empty()) { return; }

    uint32_t strikes = ++_slowCounts[path];
    if (strikesThreshold > 0 && strikes >= strikesThreshold)
    {
        _slowCounts.erase(path);
        SaveSlowCounts();
        AddToBlocklist(path);
        projectm_playlist_play_next(_playlist, true);
        ApplyBlocklist(); // NOTE: Task 4 renames this to ApplyRemovalLists()
        poco_information_f2(_logger, "Auto-skip: preset too slow %?u times — blocklisted: %s",
                            strikes, path);
    }
    else
    {
        SaveSlowCounts();
        projectm_playlist_play_next(_playlist, true);
        poco_information_f2(_logger, "Auto-skip: slow preset (strike %?u): %s", strikes, path);
    }
}
```

- [ ] **Step 4: Add autoskip timing state to RenderLoop**

In `external/frontend-sdl-cpp/src/RenderLoop.h`, add `#include <cstdint>` near the top includes, and after `int _renderHeight{0};` (line 94) add:

```cpp
    uint32_t _lastPlaylistPos{0xFFFFFFFFu}; //!< Last seen playlist position (preset-change detection).
    uint32_t _presetStartTicks{0};          //!< SDL ticks when the current preset started (grace window).
    uint32_t _lowFpsStartTicks{0};          //!< SDL ticks when FPS first dropped below threshold (0 = not low).
```

- [ ] **Step 5: Capture status once and add the autoskip check in RenderLoop::Run**

In `external/frontend-sdl-cpp/src/RenderLoop.cpp`, the current line 64 is:

```cpp
        _remoteControl.PublishStatus(_projectMWrapper.CurrentStatus(), _audioCapture.AudioDeviceName(), limiter.FPS());
```

Replace it with a captured status, preset-change detection, and the publish call:

```cpp
        auto status = _projectMWrapper.CurrentStatus();
        if (status.position != _lastPlaylistPos)
        {
            _lastPlaylistPos = status.position;
            _presetStartTicks = SDL_GetTicks();
            _lowFpsStartTicks = 0; // fresh preset — reset the low-FPS window
        }
        _remoteControl.PublishStatus(status, _audioCapture.AudioDeviceName(), limiter.FPS());
```

Then, immediately after the watchdog block (after the closing brace of `if (SDL_GetTicks() - renderStart > 4000) { … }`, old lines 69-73), add the autoskip check:

```cpp
        // Low-FPS autoskip: if a preset runs below the threshold for a sustained window (after a
        // grace period), skip it. A persistent per-preset strike counter blocklists chronic offenders.
        if (_userConfig->getBool("autoskipEnabled", true))
        {
            uint32_t now = SDL_GetTicks();
            if (now - _presetStartTicks > 2000) // grace: ignore first-frame compile / soft-cut
            {
                float fps = limiter.FPS();
                double threshold = _userConfig->getDouble("autoskipFps", 20.0);
                if (fps > 0.5f && fps < threshold)
                {
                    if (_lowFpsStartTicks == 0) { _lowFpsStartTicks = now; }
                    else if (now - _lowFpsStartTicks > 1500) // sustained low FPS
                    {
                        int strikes = _userConfig->getInt("autoskipStrikes", 3);
                        _projectMWrapper.AutoSkipSlow(static_cast<uint32_t>(strikes < 0 ? 0 : strikes));
                        _lowFpsStartTicks = 0;
                        _presetStartTicks = now; // grace the next preset
                    }
                }
                else
                {
                    _lowFpsStartTicks = 0; // recovered
                }
            }
        }
```

- [ ] **Step 6: Add autoskip settings keys to RemoteControl**

In `external/frontend-sdl-cpp/src/RemoteControl.cpp`:

Extend the `kKeys` set (from Task 2) to also include the autoskip keys — replace the closing line of the set:
```cpp
            "brightness", "tintEnabled", "tintColor", "tintStrength"};
```
with:
```cpp
            "brightness", "tintEnabled", "tintColor", "tintStrength",
            "autoskipEnabled", "autoskipFps", "autoskipStrikes"};
```

In `ApplySetting`, after the `tintStrength` branch (Task 2), add:
```cpp
    else if (key == "autoskipEnabled") { app.UserConfiguration()->setBool("projectM.autoskipEnabled", on); }
    else if (key == "autoskipFps") { app.UserConfiguration()->setDouble("projectM.autoskipFps", v); }
    else if (key == "autoskipStrikes") { app.UserConfiguration()->setInt("projectM.autoskipStrikes", static_cast<int>(v)); }
```

In `PublishStatus`, extend the settings snapshot — change the `tintStrength` line (Task 2) to end with a comma and append:
```cpp
                 << "\"tintStrength\":" << ProjectMSDLApplication::instance().config().getDouble("projectM.tintStrength", 1.0) << ","
                 << "\"autoskipEnabled\":" << (ProjectMSDLApplication::instance().config().getBool("projectM.autoskipEnabled", true) ? "true" : "false") << ","
                 << "\"autoskipFps\":" << ProjectMSDLApplication::instance().config().getDouble("projectM.autoskipFps", 20.0) << ","
                 << "\"autoskipStrikes\":" << ProjectMSDLApplication::instance().config().getInt("projectM.autoskipStrikes", 3)
                 << "}";
```

- [ ] **Step 7: Add the autoskip Settings UI**

In `remote/index.html`, after the Screen-tint `<div class="set">…</div>` block (from Task 2), insert:

```html
    <div class="set">
      <div class="tg"><b>Auto-skip slow presets</b><div class="sw" id="tAutoskip"></div></div>
      <div class="desc">Skip presets that run below the FPS threshold; repeat offenders get blocked.</div>
      <div class="h mt"><span class="label">FPS threshold</span><span class="readout" id="vAskFps">—</span></div>
      <input type="range" id="sAskFps" data-key="autoskipFps" min="5" max="60" step="5">
      <div class="h mt"><span class="label">Strikes before block</span><span class="readout" id="vAskStr">—</span></div>
      <input type="range" id="sAskStr" data-key="autoskipStrikes" min="1" max="10" step="1">
    </div>
```

- [ ] **Step 8: Wire the autoskip controls in app.js**

In `remote/app.js`, in `loadSettings()` add (after the tint lines from Task 2):

```javascript
    $("tAutoskip").classList.toggle("on", !!s.autoskipEnabled);
    $("sAskFps").value = s.autoskipFps; $("vAskFps").textContent = s.autoskipFps;
    $("sAskStr").value = s.autoskipStrikes; $("vAskStr").textContent = s.autoskipStrikes;
```

Add `sAskFps`/`sAskStr` to the integer `valueLabel` map so they post on change and update their labels — change the `valueLabel` definition (from Task 2) to:

```javascript
const valueLabel = { sDur: "vDur", sSoft: "vSoft", sBeat: "vBeat", sHardS: "vHardS", sHardD: "vHardD", sFps: "vFps", sFlash: "vFlash", sAskFps: "vAskFps", sAskStr: "vAskStr" };
```

And add the toggle handler (near the other toggles):

```javascript
$("tAutoskip").onclick = () => {
  const on = !$("tAutoskip").classList.contains("on");
  $("tAutoskip").classList.toggle("on", on);
  POST(`/api/settings?key=autoskipEnabled&value=${on}`);
};
```

- [ ] **Step 9: Build, deploy, and verify on the Pi**

```bash
./scripts/build.sh
cp -r remote/. "$HOME/.local/share/dropkick/remote/"
```
Relaunch. Force a skip by setting the threshold above the real FPS (e.g. 60) so ordinary presets trip it:
```bash
curl -s -X POST "http://sleempie.local:8080/api/settings?key=autoskipFps&value=60"
curl -s -X POST "http://sleempie.local:8080/api/settings?key=autoskipStrikes&value=3"
```
Watch the log — expect repeated `Auto-skip: slow preset (strike 1/2)` then `… blocklisted` on the 3rd strike for a given preset, and:
```bash
cat "$HOME/.local/share/dropkick/slowcounts.txt"   # shows "<count>\t<path>" lines
curl -s "http://sleempie.local:8080/api/status" | tr ',' '\n' | grep blocked  # rises as presets get blocked
```
Restart the app and confirm `slowcounts.txt` counts survived (persistence). Then set `autoskipFps` back to 20.

- [ ] **Step 10: Commit**

```bash
git add external/frontend-sdl-cpp/src/ProjectMWrapper.h external/frontend-sdl-cpp/src/ProjectMWrapper.cpp \
        external/frontend-sdl-cpp/src/RenderLoop.h external/frontend-sdl-cpp/src/RenderLoop.cpp \
        external/frontend-sdl-cpp/src/RemoteControl.cpp remote/index.html remote/app.js
git commit -m "$(cat <<'EOF'
feat(playback): low-FPS autoskip with persistent per-preset strike counter

Skips presets sustained below the FPS threshold after a grace window; N strikes
(default 3) blocklists a preset. Threshold/strikes/enable exposed in the remote.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01PGfsm6gg5JXx1LSotdT9k9
EOF
)"
```

---

## Task 4: Dislike button (separate list)

A dislike list distinct from the GPU-hang blocklist, triggered on the now-playing preset from the remote.

**Files:**
- Modify: `external/frontend-sdl-cpp/src/ProjectMWrapper.h` — public dislike API; private dislike members/methods; rename `ApplyBlocklist`→`ApplyRemovalLists`
- Modify: `external/frontend-sdl-cpp/src/ProjectMWrapper.cpp` — init/load, dislike methods, rename + generalize `ApplyBlocklist`, add filter call in `LoadPresetPack`
- Modify: `external/frontend-sdl-cpp/src/RemoteControl.h` — new command types
- Modify: `external/frontend-sdl-cpp/src/RemoteControl.cpp` — routes, drain cases, status `disliked`
- Modify: `remote/index.html`, `remote/app.js` — dislike button + disliked row

**Interfaces:**
- Produces (`ProjectMWrapper`): `void DislikeCurrent()`, `void ClearDislikes()`, `uint32_t DislikedCount() const`, `void ApplyRemovalLists()` (was `ApplyBlocklist`).
- Produces (`RemoteControl`): `POST /api/dislike`, `POST /api/dislikes/clear`; status JSON field `disliked`.

- [ ] **Step 1: Declare the dislike API and rename ApplyBlocklist in the header**

In `external/frontend-sdl-cpp/src/ProjectMWrapper.h`:

After `void ClearBlocklist();` (line 136), add:

```cpp
    /** @brief Adds the currently-playing preset to the dislike list, removes it, and advances. */
    void DislikeCurrent();

    /** @brief Number of presets on the dislike list. */
    uint32_t DislikedCount() const;

    /** @brief Clears the dislike list (disliked presets return on the next pack load/restart). */
    void ClearDislikes();
```

Rename the private declaration (line 165) from:
```cpp
    void ApplyBlocklist();                         //!< Remove blocklisted entries from the playlist.
```
to:
```cpp
    void ApplyRemovalLists();                      //!< Remove blocklisted OR disliked entries from the playlist.
    void LoadDislikes();                           //!< Read the dislike file into memory.
    void AddToDislikes(const std::string& path);   //!< Add a path to the dislike list (memory + file).
```

After the `_blocklistPath` member (line 172), add:
```cpp
    std::set<std::string> _dislikes;    //!< Preset paths the user disliked.
    std::string _dislikePath;           //!< ~/.local/share/dropkick/dislikes.txt
```

- [ ] **Step 2: Rename ApplyBlocklist and generalize it in the .cpp**

In `external/frontend-sdl-cpp/src/ProjectMWrapper.cpp`, replace the whole `ApplyBlocklist` definition (lines 337-356) with:

```cpp
void ProjectMWrapper::ApplyRemovalLists()
{
    if (!_playlist || (_blocklist.empty() && _dislikes.empty())) { return; }
    uint32_t size = projectm_playlist_size(_playlist);
    uint32_t removed = 0;
    for (uint32_t i = size; i-- > 0;) // high->low so indices stay valid while removing
    {
        char* item = projectm_playlist_item(_playlist, i);
        if (item)
        {
            if (_blocklist.count(item) || _dislikes.count(item))
            {
                projectm_playlist_remove_preset(_playlist, i);
                ++removed;
            }
            projectm_playlist_free_string(item);
        }
    }
    if (removed)
    {
        poco_information_f2(_logger, "Removal lists: dropped %?u presets (%?u blocked, disliked lists applied).",
                            removed, static_cast<uint32_t>(_blocklist.size()));
    }
}
```

- [ ] **Step 3: Update all ApplyBlocklist call sites and add dislike methods**

In `external/frontend-sdl-cpp/src/ProjectMWrapper.cpp`:

- Line 117 (`initialize`): change `ApplyBlocklist();` → `ApplyRemovalLists();`
- Line ~398 (`QuarantineCurrent`): change `ApplyBlocklist();` → `ApplyRemovalLists();`
- If Task 3 already landed, its `AutoSkipSlow` call site `ApplyBlocklist();` → `ApplyRemovalLists();`

In `LoadPresetPack`, after the sort block (after line 251's closing `}`), add a filter call so dislikes/blocks persist across pack switches:

```cpp
    ApplyRemovalLists(); // keep blocked/disliked presets out after a pack switch
```

In `initialize`, after `LoadBlocklist();` (line 36), add:

```cpp
    _dislikePath = Poco::Path::expand("~/.local/share/dropkick/dislikes.txt");
    LoadDislikes();
```

After the `ClearBlocklist()` definition (and any Task 3 methods), add the dislike methods:

```cpp
void ProjectMWrapper::LoadDislikes()
{
    _dislikes.clear();
    std::ifstream in(_dislikePath);
    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        if (!line.empty()) { _dislikes.insert(line); }
    }
}

void ProjectMWrapper::AddToDislikes(const std::string& path)
{
    if (path.empty() || _dislikes.count(path)) { return; }
    _dislikes.insert(path);
    std::ofstream out(_dislikePath, std::ios::app);
    if (out) { out << path << "\n"; }
    poco_information_f1(_logger, "Disliked preset: %s", path);
}

void ProjectMWrapper::DislikeCurrent()
{
    if (!_playlist) { return; }
    uint32_t pos = projectm_playlist_get_position(_playlist);
    char* item = projectm_playlist_item(_playlist, pos);
    std::string path = item ? item : "";
    if (item) { projectm_playlist_free_string(item); }
    if (path.empty()) { return; }

    AddToDislikes(path);
    projectm_playlist_play_next(_playlist, true); // move off it first
    ApplyRemovalLists();                          // then drop it (and any others)
}

uint32_t ProjectMWrapper::DislikedCount() const
{
    return static_cast<uint32_t>(_dislikes.size());
}

void ProjectMWrapper::ClearDislikes()
{
    _dislikes.clear();
    std::remove(_dislikePath.c_str());
    poco_information(_logger, "Dislike list cleared (disliked presets return on next pack load/restart).");
}
```

- [ ] **Step 4: Add the command types**

In `external/frontend-sdl-cpp/src/RemoteControl.h`, extend the `CommandType` enum (lines 32-36) to add the two new types:

```cpp
    enum class CommandType
    {
        Next, Previous, Random, ToggleShuffle, ToggleLock, NextAudio, LoadPack,
        SetPosition, SetSetting, CaptureWorkshop, ClearBlocklist, LoadWorkshopPath,
        DislikeCurrent, ClearDislikes
    };
```

- [ ] **Step 5: Add the routes and drain cases**

In `external/frontend-sdl-cpp/src/RemoteControl.cpp`, in `RegisterRoutes`, after the `/api/blocklist/clear` route (ends ~line 241), add:

```cpp
    _server->Post("/api/dislike", [this, guard](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) { return; }
        Enqueue(Command{CommandType::DislikeCurrent, "", ""});
        res.set_content("{\"ok\":true}", "application/json");
    });

    _server->Post("/api/dislikes/clear", [this, guard](const httplib::Request& req, httplib::Response& res) {
        if (!guard(req, res)) { return; }
        Enqueue(Command{CommandType::ClearDislikes, "", ""});
        res.set_content("{\"ok\":true}", "application/json");
    });
```

In `DrainCommands`, add two cases to the `switch` (e.g. after `case CommandType::ClearBlocklist:` block, ~line 408):

```cpp
            case CommandType::DislikeCurrent:
                _workshopActive = false;
                app.getSubsystem<ProjectMWrapper>().DislikeCurrent();
                break;
            case CommandType::ClearDislikes:
                app.getSubsystem<ProjectMWrapper>().ClearDislikes();
                break;
```

- [ ] **Step 6: Report the disliked count in status**

In `PublishStatus`, in the status JSON (after the `"blocked"` line ~655), add:

```cpp
         << "\"disliked\":" << ProjectMSDLApplication::instance().getSubsystem<ProjectMWrapper>().DislikedCount() << ","
```

- [ ] **Step 7: Add the dislike button and disliked row to the UI**

In `remote/index.html`, in the NOW view, after the workshop row (`</div>` at line 46), add a dislike action row:

```html
      <div class="workshoprow">
        <button class="wsbtn danger" id="btnDislike">👎 Dislike — stop playing this</button>
      </div>
```

In the SETTINGS view, after the blocked-presets row (`</div>` at line 113), add:

```html
    <div class="set flexrow" id="dislikedRow" style="display:none">
      <div><b>Disliked presets</b><div class="desc" style="margin:5px 0 0">Presets you disliked. Cleared ones return on the next pack switch.</div></div>
      <button class="readout btnish" id="btnClearDislikes"><span id="dislikedCount">0</span> · Clear</button>
    </div>
```

In `remote/style.css`, append a subtle style for the danger button:

```css
.wsbtn.danger { color: #ff6b6b; }
```

- [ ] **Step 8: Wire the dislike controls in app.js**

In `remote/app.js`, in `refresh()`, after the blocked lines (~139-141), add:

```javascript
    const disliked = s.disliked || 0;
    $("dislikedRow").style.display = disliked > 0 ? "" : "none";
    $("dislikedCount").textContent = disliked;
```

Near the other button handlers (after `btnClearBlock`, ~line 191), add:

```javascript
$("btnDislike").onclick = () => { POST("/api/dislike"); setTimeout(refresh, 500); };
$("btnClearDislikes").onclick = () => { POST("/api/dislikes/clear"); setTimeout(refresh, 500); };
```

- [ ] **Step 9: Build, deploy, and verify on the Pi**

```bash
./scripts/build.sh
cp -r remote/. "$HOME/.local/share/dropkick/remote/"
```
Relaunch, then:
```bash
before=$(curl -s "http://sleempie.local:8080/api/status" | tr ',' '\n' | grep '"size"')
curl -s -X POST "http://sleempie.local:8080/api/dislike"
sleep 1
curl -s "http://sleempie.local:8080/api/status" | tr ',' '\n' | grep -E '"size"|disliked'
cat "$HOME/.local/share/dropkick/dislikes.txt"
```
Expected: `{"ok":true}`; `disliked` increments; playlist `size` drops by 1; the disliked preset's path is in `dislikes.txt`; the on-screen preset advances. In the remote, the 👎 button on Now works and Settings shows "N disliked · Clear". Test Clear:
```bash
curl -s -X POST "http://sleempie.local:8080/api/dislikes/clear"
```
Then switch packs (or restart) and confirm cleared presets can return while still-listed ones stay gone.

- [ ] **Step 10: Commit**

```bash
git add external/frontend-sdl-cpp/src/ProjectMWrapper.h external/frontend-sdl-cpp/src/ProjectMWrapper.cpp \
        external/frontend-sdl-cpp/src/RemoteControl.h external/frontend-sdl-cpp/src/RemoteControl.cpp \
        remote/index.html remote/app.js remote/style.css
git commit -m "$(cat <<'EOF'
feat(playback): dislike button with a separate dislike list

Adds dislikes.txt (distinct from the safety blocklist), POST /api/dislike +
/api/dislikes/clear, a Now-view button and a Settings count/Clear. Playlist
filtering now removes blocked OR disliked presets, including on pack switch.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01PGfsm6gg5JXx1LSotdT9k9
EOF
)"
```

---

## Task 5: Boot config defaults

Document and default the new `projectM.*` keys in the runtime config template so a fresh install starts sane and the values are discoverable/editable. (Code already defaults these; this makes them explicit in the rendered properties file.)

**Files:**
- Modify: `config/projectMSDL.properties.in`

**Interfaces:** none (static config text). `sync-config.sh` copies non-`DROPKICK_*` lines through verbatim.

- [ ] **Step 1: Append the new defaults to the runtime template**

In `config/projectMSDL.properties.in`, add a section (place it after the existing `projectM.aspectCorrectionEnabled` line, mirroring the existing `projectM.` style):

```properties

### Dropkick visual controls & playback quality

# Reduce flashing (temporal persistence). Strength 0..1 (higher = smoother/more trails).
projectM.reduceFlashing = false
projectM.flashStrength = 0.6

# Screen brightness multiplier, 0..1 (1 = full brightness).
projectM.brightness = 1.0

# Monochrome screen tint. tintColor is #rrggbb; tintStrength 0..1 (1 = full monochrome).
projectM.tintEnabled = false
projectM.tintColor = #00ff00
projectM.tintStrength = 1.0

# Auto-skip presets that run below autoskipFps. After autoskipStrikes slow skips, a preset is blocklisted.
projectM.autoskipEnabled = true
projectM.autoskipFps = 20
projectM.autoskipStrikes = 3
```

> Note: `reduceFlashing`/`flashStrength` may already be documented elsewhere; if those two lines already exist in this file, add only the brightness/tint/autoskip lines and skip the duplicates.

- [ ] **Step 2: Render and verify on the Pi**

```bash
./scripts/sync-config.sh
grep -E 'brightness|tint|autoskip' "$HOME/.local/projectMSDL.properties"
```
Expected: the new keys appear in the rendered runtime properties file. Restart Dropkick and confirm defaults load (brightness full, tint off, autoskip on) via `curl -s http://sleempie.local:8080/api/settings`.

- [ ] **Step 3: Commit**

```bash
git add config/projectMSDL.properties.in
git commit -m "$(cat <<'EOF'
chore(config): default and document new visual-control keys

Adds brightness, tint, and autoskip defaults to the runtime properties template.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01PGfsm6gg5JXx1LSotdT9k9
EOF
)"
```

---

## Final integration check (on the Pi)

After all tasks:
- `./scripts/build.sh && cp -r remote/. "$HOME/.local/share/dropkick/remote/" && ./scripts/sync-config.sh`
- Relaunch; open the remote. Confirm end-to-end: strobe smoothing visibly stronger; brightness slider dims; tint + swatches + intensity recolor; 👎 removes the current preset and it's listed in Settings; auto-skip trips at a raised threshold and blocklists at 3 strikes; all counts (`blocked`, `disliked`) render.
- Confirm no GPU crash across ~10 minutes of playback with effects on.
