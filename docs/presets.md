# Presets & Textures

## Layout
Dropkick organizes presets into **packs** — subfolders of a library root:

    ~/.local/share/dropkick/presets/
      cream-of-the-crop/
      my-picks/
    ~/.local/share/dropkick/textures/

## Config keys (`projectMSDL.properties`)
- `projectM.presetPath` — active pack directory loaded at startup.
- `projectM.texturePath` — texture root.

Override the library root via `DROPKICK_PRESET_ROOT` / `DROPKICK_TEXTURE_ROOT`
in `config/dropkick.env` (see `docs/setup.md`).

## Switching packs at runtime
The phone remote's **Packs** control lists the subfolders of the library root
and reloads the playlist from the selected pack (`POST /api/pack`).
