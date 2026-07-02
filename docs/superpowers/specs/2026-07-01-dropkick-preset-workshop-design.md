# Dropkick Preset Workshop — Design Spec

**Date:** 2026-07-01 · **Status:** Approved (design agreed in chat)

## Goal
Let the user author their own Milkdrop `.milk` presets with a tight edit-and-see loop: edit a file in any editor, and Dropkick live-reloads it on the TV. Plus a way to grab the currently-playing preset to riff on, and starter/reference material for people new to the format.

## Features
1. **Workshop folder + hot-reload.** A watched directory `~/.local/share/dropkick/workshop/`. Each render-loop drain (~1×/sec, throttled), the render thread stat()s the folder; if any `.milk`'s mtime changed since last seen (or a new one appeared), it loads that file live via `projectm_load_preset_file(handle, path, true /*smooth*/)`. The most-recently-modified `.milk` wins when several change.
2. **Send current → workshop.** `POST /api/workshop/capture` copies the currently-playing preset's file into `workshop/` (filename deduped if it exists), then relies on the watcher to load the copy. Surfaced as a button on the remote's Now Playing view. The status snapshot already carries the current preset path.
3. **Starter + docs (D).** `bootstrap.sh` seeds `workshop/` with `starter.milk` (a minimal, heavily-commented preset). `docs/authoring.md` documents the workshop loop, the common per-frame variables (`bass`/`mid`/`treb`/`bass_att`, `time`, `q1`–`q32`, `zoom`/`rot`/`warp`/`decay`), the GLES shader caveat, and 3–4 "change this line → see this" recipes.

## API (RemoteControl)
- `POST /api/workshop/capture` → enqueues a `CaptureWorkshop` command; the render-thread drain reads the current preset path (from the last published status / playlist position), copies the file into the workshop dir, and lets the watcher pick it up. Returns `{"ok":true,"file":"<name>"}` or `{"error":...}` if there's no current file.
- `GET /api/status` gains `"workshop":bool` — true when the active preset was loaded from the workshop folder (so the remote can show a "live-editing" indicator).

## Implementation notes
- **Watcher:** a `WorkshopWatcher` helper owned by `RemoteControl` (or a small method set): holds `dirPath`, a `map<string,time_t>` of known files+mtimes, and a throttle timestamp. `Poll()` runs in `DrainCommands()` (render thread — safe to call projectM). Uses `opendir`/`stat`, same style as `PacksJson`. First poll seeds the map without loading (so startup doesn't hijack the current preset).
- **Load:** `projectm_load_preset_file` on the ProjectM handle (`ProjectMWrapper::ProjectM()`); wrap in a `ProjectMWrapper::LoadPresetFile(path)` for tidiness. Loading a raw file this way plays it immediately without adding to the playlist — acceptable for a scratch/preview loop.
- **Capture:** plain file copy (ifstream→ofstream). Dedup by appending ` (n)` before `.milk`.
- **Config:** `remote.workshopDir` (default `~/.local/share/dropkick/workshop`).
- **Thread-safety:** watcher + load + capture all run on the render thread inside the drain; HTTP handlers only enqueue (capture) — consistent with the existing model.

## Out of scope
In-remote text editor (that's a later feature); adding workshop presets to the main playlist automatically; validation of preset syntax (projectM already logs parse issues).
