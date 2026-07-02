# Dropkick Remote v2 — Design Spec

**Date:** 2026-07-01 · **Status:** Approved (user picked visual direction "Console" + dark mode)

## Goal
Expand the phone remote into a four-view app: **Now Playing / Browse / Favorites / Settings**, in the "Console" visual direction (warm light panel, single orange accent, hardware-style keys, segmented readouts — Space Grotesk + JetBrains Mono) with a **dark mode** (default follows `prefers-color-scheme`, manual toggle persisted in localStorage).

## Features
1. **Preset list (Browse):** full playlist (~9.8k) fetched once via `GET /api/presets`; client groups by category (first path segment after the common prefix), categories collapsed by default; search box filters across all (up to 300 rendered matches). Tap a row → jump to that preset. Star on each row. *Thumbnails deferred to a later project.*
2. **Favorites:** starred presets persisted server-side to `~/.local/share/dropkick/favorites.json` (JSON array of preset paths — stable across restarts). Favorites view lists stars (tap to jump), plus **Shuffle favorites** mode: when on, Next picks the next favorite in playlist order, Random picks a random favorite. Empty favorites → mode is a no-op.
3. **Name vs path:** Now Playing shows the display name (basename, `.milk` stripped); tapping the readout reveals the full path. Category shown as a chip.
4. **Settings:** live values from `GET /api/settings`, applied via `POST /api/settings` — preset duration, soft-cut (transition) duration, hard cut on/off + duration + sensitivity, beat sensitivity, FPS (updates both `projectm_set_fps` and the frontend limiter's `projectM.fps` config), aspect correction.

## API (RemoteControl, in-process)
All mutations enqueue commands drained on the render thread; all reads come from mutex-guarded snapshots refreshed on the render thread. Token guard applies to all `/api/*`.

- `GET /api/presets` → `[{"i":N,"p":"path"},…]` — cached JSON, rebuilt on render thread at startup and after pack load.
- `POST /api/preset?index=N` → `projectm_playlist_set_position`.
- `GET /api/favorites` → JSON array of paths. `POST /api/favorites/toggle?path=…` → add/remove + save file (HTTP-thread safe: favorites set has its own mutex, no projectM access).
- `POST /api/favorites/shuffle` → toggle mode (atomic bool).
- `GET /api/settings` / `POST /api/settings?key=…&value=…` (keys: `presetDuration`, `softCutDuration`, `hardCut`, `hardCutDuration`, `hardCutSensitivity`, `beatSensitivity`, `fps`, `aspectCorrection`).
- `GET /api/status` extended with `"favorited":bool` and `"favoritesShuffle":bool`.

## Implementation notes
- `ProjectMWrapper::PlaylistItems()` (new) wraps `projectm_playlist_items` / `projectm_playlist_free_string_array`.
- Preset cache + path→index map rebuilt in `DrainCommands` when a dirty flag is set; settings JSON rebuilt in `PublishStatus` (render thread) via projectM getters.
- Favorites file format: JSON string array using the existing `JsonEscape`; parsed with a minimal quoted-string scanner.
- UI stays three static files (`remote/index.html`, `app.js`, `style.css`), no framework.

## Out of scope
Thumbnails (separate render-harness project); multi-user auth; preset editing.
