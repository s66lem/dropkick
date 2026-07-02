# Dropkick GPU-Hang Auto-Skip — Design Spec

**Date:** 2026-07-01 · **Status:** Approved (build now)

## Problem
The Pi's V3D GPU hangs on some shader-heavy presets (`v3d Resetting GPU for hang`).
Observed behavior: the **app process dies**. Shuffle across a 9.8k pack keeps hitting
these, so the visualizer is unstable. We need it to survive bad presets and stop
showing the ones that kill it.

## Design
Because the app dies (not just a recoverable GL error), the reliable core is a
**supervisor + crash-quarantine**, with an in-process watchdog as a best-effort
fast path for hangs the app survives.

### 1. Supervisor (launch-agnostic)
`dropkick` launcher becomes a restart loop: run `projectMSDL`; on **clean exit (0)**
stop; on **failure (non-zero/signal)** relaunch. Rate guard: if it fails ≥5 times in
60s, stop and log (prevents a fast crash loop). Works whether launched from the menu
or a terminal.

### 2. Crash breadcrumb
The app records the **active preset path** to `~/.local/share/dropkick/state/loading`
whenever a preset becomes active (`ProjectMWrapper::PresetSwitchedEvent`). On **clean
shutdown** (`uninitialize`) it deletes the file.

### 3. Startup quarantine
On `ProjectMWrapper::initialize`: if `state/loading` exists (the previous run died
while that preset was live), append its path to `~/.local/share/dropkick/blocklist.txt`
and delete `state/loading`. So the preset that killed the app gets quarantined on the
auto-restart.

### 4. Blocklist filtering
After presets are loaded into the playlist (initial load **and** `LoadPresetPack`),
remove any entry whose path is in `blocklist.txt` (walk the playlist,
`projectm_playlist_remove_preset` for matches — iterate high→low index). Blocked
presets never play again.

### 5. In-process watchdog (best-effort)
In `RenderLoop`, time each `RenderFrame()`. If a frame exceeds **3.0s** (a hang the
app survived), quarantine the current preset (append to blocklist + breadcrumb clear)
and advance (`play_next`). Catches survivable hangs without a restart. 3s is well above
a legit heavy first-frame (shader compile) so false positives are rare.

### 6. Remote surface (light)
- `GET /api/status` gains `"blocked":N` (blocklist size).
- `POST /api/blocklist/clear` empties the blocklist (and reloads the pack so cleared
  presets return).
- Settings shows "N presets blocked — Clear" when N > 0.

## Files
- `scripts/dropkick-launcher.sh` — supervisor loop + rate guard.
- `ProjectMWrapper.{h,cpp}` — breadcrumb, startup quarantine, blocklist load/filter,
  `QuarantineCurrent()` helper, `BlockedCount()`.
- `RenderLoop.cpp` — frame watchdog.
- `RemoteControl.{h,cpp}` — `blocked` in status, `/api/blocklist/clear`.
- `remote/` — blocked line + Clear button in Settings.

## Data files
- `~/.local/share/dropkick/state/loading` — breadcrumb (transient).
- `~/.local/share/dropkick/blocklist.txt` — one preset path per line (persistent).

## Out of scope
Per-preset automatic classification beyond "it killed/hung the app"; a browsable
blocklist UI (just a count + clear); distinguishing GPU-hang from other crash causes
(any crash quarantines the active preset — acceptable, user can Clear).
