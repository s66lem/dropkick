# Dropkick — Visual Controls & Playback Quality

**Date:** 2026-07-02
**Status:** Approved design, pending implementation plan

## Summary

Five additions to the Dropkick visualizer, driven from the phone remote:

1. **Dislike button** — banish the current preset from playback (separate from the safety blocklist).
2. **Strobe dampener fix** — the existing "reduce flashing" filter barely bites, especially on rapid color cycling. Replace its per-channel clamp with a temporal persistence blend.
3. **Low-FPS autoskip** — skip presets that run too slowly; a per-preset strike counter blocklists chronic offenders.
4. **Brightness slider** — dim the whole screen from the remote.
5. **Monochrome tint** — recolor the entire screen to a single hue (green, blue, pink, …).

Features 2, 4, and 5 are all fullscreen post-processing effects and share one render pass.

## Architectural spine: a unified post-process pass

The frontend already renders projectM into an offscreen framebuffer and runs a fullscreen GLES shader to the backbuffer — that is `StrobeFilter` (`external/frontend-sdl-cpp/src/StrobeFilter.{h,cpp}`). Brightness, monochrome tint, and the improved strobe reduction are all per-pixel fullscreen operations, so they belong in that same pass.

**Change:** evolve `StrobeFilter` into a `PostProcess` class (rename the files to `PostProcess.{h,cpp}`; update `RenderLoop` and `CMakeLists.txt`). It keeps the proven FBO lifecycle (`EnsureResources` / `Destroy`, GLES-3.1-only, no-op stub on desktop builds) and the single-pass `glCopyTexSubImage2D` feedback trick. The composite fragment shader becomes:

```glsl
vec3 scene    = texture(uScene, vUV).rgb;
vec3 prev     = texture(uPrev,  vUV).rgb;
vec3 smoothed = mix(scene, prev, uPersistence);   // strobe reduction
vec3 c        = smoothed * uBrightness;            // brightness
if (uTintEnabled > 0.5) {
    float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = lum * uTintColor;                          // true monochrome
}
frag = vec4(c, 1.0);
```

After drawing to the backbuffer, the displayed frame is copied into `uPrev` for the next frame's persistence term (unchanged mechanism). Persistence operating in displayed (post-tint) space is acceptable: changing brightness/tint causes a brief settle, then converges — visually harmless.

**Activation predicate:** the pass runs only when at least one effect is active — `reduceFlashing || brightness != 1.0 || tintEnabled`. When none are active, `RenderLoop` renders straight to the backbuffer exactly as today (no FBO round-trip, no perf cost, desktop builds unaffected).

**RenderLoop wiring:** each frame, read the config values and push them into `PostProcess` (same pattern already used for `reduceFlashing`/`flashStrength`):

```cpp
_post.SetReduceFlashing(cfg->getBool("projectM.reduceFlashing", false));
_post.SetStrength(cfg->getDouble("projectM.flashStrength", 0.6));
_post.SetBrightness(cfg->getDouble("projectM.brightness", 1.0));
_post.SetTint(cfg->getBool("projectM.tintEnabled", false),
              cfg->getString("projectM.tintColor", "#00ff00"));
if (_post.Active()) { _post.Begin(w, h); RenderFrame(_post.SceneFbo()); _post.Composite(); }
else                { RenderFrame(); }
```

`PostProcess` parses the tint hex string to a normalized `vec3` internally.

## Feature 1 — Strobe dampener fix (persistence blend)

**Problem.** `StrobeFilter::Composite()` clamps each channel's per-frame delta to `maxDelta = max(0.02, 1.0 - strength)`. At the default strength 0.6 that is 0.4 — a pixel may still swing 40% brightness per frame, so a full black↔white flash finishes in ~2.5 frames and rapid hue cycling is barely touched. The slider only bites near its maximum.

**Fix.** Replace the hard clamp with an exponential temporal blend toward the previous displayed frame:

```
out = mix(scene, prev, k),  where k = 0.9 * flashStrength   // k in [0, 0.9]
```

This is a temporal low-pass on the entire signal, so it smooths both brightness flashes and rapid red→green→blue cycling. At the default strength 0.6, `k = 0.54` (each frame ≈ 46% new / 54% carried over) — clearly visible damping, unlike today. `k` is recalibrated on the Pi against real presets.

The existing "Reduce flashing" toggle and strength slider in the remote drive this unchanged (`projectM.reduceFlashing`, `projectM.flashStrength`). When reduce-flashing is off, `k = 0` (no persistence) but brightness/tint may still be active.

## Feature 2 — Brightness slider

- **Config:** `projectM.brightness`, float 0.0–1.0, default 1.0.
- **Render:** post-process multiply (`smoothed * uBrightness`). Values < 1.0 activate the pass.
- **Remote:** a 0–100% slider in Settings, wired through the settings API like the other numeric settings.

## Feature 3 — Monochrome tint

- **Config:** `projectM.tintEnabled` (bool, default false) and `projectM.tintColor` (hex string, default `#00ff00`).
- **Render:** collapse the pixel to Rec. 709 luminance, then scale the chosen color by it → true monochrome (shades of one hue). Enabling activates the pass.
- **Remote:** in Settings — an on/off toggle, a native `<input type="color">` picker, and quick swatches: green, blue, pink, amber, cyan, red, white, plus "off". Selecting a swatch sets both the color and the toggle.

## Feature 4 — Dislike button (separate list)

A dislike list distinct from the GPU-hang safety blocklist: "not my taste" vs "crashes the Pi" stay separate, each with its own count and Clear control.

**ProjectMWrapper** gains a parallel set mirroring the blocklist code:
- `_dislikes` set + `_dislikePath = ~/.local/share/dropkick/dislikes.txt`.
- `LoadDislikes()`, `AddToDislikes(path)`, `DislikeCurrent()`, `ClearDislikes()`, `DislikedCount()`.
- `DislikeCurrent()` mirrors `QuarantineCurrent()`: add the current preset's path to `dislikes.txt`, advance off it (`projectm_playlist_play_next`), then filter it out.
- Playlist filtering at pack load removes any preset in **blocklist ∪ dislikes** (generalize the existing `ApplyBlocklist` removal test to check both sets, or add a parallel `ApplyDislikes` called next to it).

**RemoteControl:**
- New command `DislikeCurrent`; `POST /api/dislike` enqueues it (acts on the now-playing preset).
- `POST /api/dislikes/clear` enqueues `ClearDislikes`.
- Status JSON gains `"disliked": <count>`.

**Remote UI:**
- A 👎 button on the **Now** view next to the ♥ favorite button.
- "N disliked · Clear" in Settings, beside the existing "N blocked · Clear".

## Feature 5 — Low-FPS autoskip with strike counter

Skip presets that render too slowly; presets that are *repeatedly* slow get blocklisted so they stop coming back.

**RenderLoop** (judging happens on the render thread where FPS is known):
- Track a smoothed FPS from `limiter.FPS()`.
- Detect preset changes by watching the playlist position; on any change, reset the low-FPS timer and stamp the new preset's entry time.
- **Grace window** (~2 s after a preset starts): no judging — covers first-frame shader compile and soft-cut transitions.
- After grace, if FPS stays continuously below the threshold for a short window (~1–2 s of sustained low FPS), trigger an auto-skip.
- Independent of the existing 4 s hard-hang watchdog (that handles freezes; this handles chronic slowness).

**Strike counter** (persistent, in `ProjectMWrapper`):
- `slowcounts.txt`: `path\tcount` per line, loaded at startup, saved on change.
- On auto-skip, `RecordSlowSkip(path)` increments that preset's count. If it reaches the strikes threshold (default 3), the preset is added to the **safety blocklist** and removed from the playlist (it was genuinely too slow, a performance problem — hence the safety list, not dislikes). Below the threshold it is only skipped and can recover on a later, faster run.

**Config:**
- `projectM.autoskipEnabled` (bool, default **true**).
- `projectM.autoskipFps` (threshold, default **20**).
- `projectM.autoskipStrikes` (default **3**).

**Remote:** Settings gets an enable toggle, an FPS-threshold input, and a strikes input.

## Config & remote plumbing (shared)

All new settings are runtime-adjustable from the remote, matching the existing settings path:
- `RemoteControl::ApplySetting` handles the new keys — `brightness`, `tintEnabled`, `tintColor`, `autoskipEnabled`, `autoskipFps`, `autoskipStrikes` — by writing the corresponding `projectM.*` key into `UserConfiguration()` (live, in-memory apply, same as `reduceFlashing`/`flashStrength`/`fps`).
- Each new key is added to the `kKeys` allow-set in the `POST /api/settings` handler.
- The settings snapshot in `PublishStatus` reports each new key so the remote reflects current state.
- Boot defaults for the new `projectM.*` keys are added to `dropkick.env` and the `projectMSDL.properties` template so a fresh install starts sane (`brightness=1`, tint off, autoskip on/20/3).

## Non-goals (YAGNI)

- No per-preset brightness/tint memory — these are global screen settings.
- No tint intensity/mix slider — tint is full monochrome when on (can be added later if wanted).
- No un-dislike-from-history UI — Clear wipes the whole dislike list, mirroring the blocklist's Clear.
- No new on-TV keybindings for these (remote-driven), except where trivial to add alongside existing keys.

## Testing & acceptance

This project builds and runs **only on the Raspberry Pi 5** (aarch64, Mesa V3D, GLES 3.1); the Windows dev host cannot compile or run it. Acceptance:

- `./scripts/bootstrap.sh` on the Pi builds clean.
- **API paths verifiable via curl:** `POST /api/dislike` (status `disliked` increments, playlist size drops), `/api/dislikes/clear`, and the new settings keys round-trip through `GET/POST /api/settings`.
- **Visual paths require eyes on the TV** and cannot be verified remotely: strobe damping strength, brightness dimming, and monochrome tint. These are validated by watching real presets.
- Low-FPS autoskip: verifiable by setting a high threshold (e.g. 60) so ordinary presets trip it, then confirming skip → strike accumulation → blocklist at 3, and `slowcounts.txt` persistence.
- Desktop (non-GLES) builds: `PostProcess` stays a no-op stub; the remote/config keys exist but the pass is inert (as `StrobeFilter` is today).
