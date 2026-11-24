# Presets & Textures

## Layout
Dropkick organizes presets into **packs** — subfolders of a library root:

    ~/.local/share/dropkick/presets/
      cream-of-the-crop/
      my-picks/
    ~/.local/share/dropkick/textures/

On Windows the root is `%USERPROFILE%\.local\share\dropkick\`.

## Config keys (`projectMSDL.properties`)
- `projectM.presetPath` — active pack directory loaded at startup.
- `projectM.texturePath` — texture root.
- `remote.presetRoot` — library root the remote scans for packs and Browse.

On Linux these are rendered from `dropkick.env`; override the library root with
`DROPKICK_PRESET_ROOT` / `DROPKICK_TEXTURE_ROOT` (see [setup.md](setup.md)).

## Switching packs at runtime
The remote's **Packs** control lists the subfolders of the library root and
reloads the playlist from the selected pack (`POST /api/pack`).

## Bad presets
Presets that hang or crawl are quarantined automatically (see
[architecture.md](architecture.md#gpu-hang-resilience)). To pre-seed the
blocklist from a static shader-size heuristic:

    ./scripts/seed-blocklist.sh --dry-run    # preview
    ./scripts/seed-blocklist.sh [threshold]  # apply (default 6000 shader bytes)

The script appends to `blocklist.txt`, which the app reads at startup: restart
Dropkick to apply it (switching packs does not reload it). The script needs its
executable bit (`chmod +x` if your checkout lost it).

Undo anytime from the remote: Settings → Blocked presets → Clear.
