# Dropkick Setup (Raspberry Pi 5)

## 1. Clone and bootstrap
    git clone --recurse-submodules https://github.com/s66lem/dropkick.git
    cd dropkick
    ./scripts/bootstrap.sh

This installs dependencies, builds both forks with `-DENABLE_GLES=TRUE`, and
installs the binary, remote web assets, config, and preset/texture dirs. It also
drops a `dropkick.env` into `~/.local/share/dropkick/` (only if one isn't there).

## 2. Add presets & textures
Drop preset packs into subfolders:

    ~/.local/share/dropkick/presets/<pack-name>/*.milk
    ~/.local/share/dropkick/textures/

## 3. Configure
Edit `~/.local/share/dropkick/dropkick.env` (audio device, preset pack,
remote port/token). Leave `DROPKICK_AUDIO_SOURCE` empty for the default device;
list devices with `projectMSDL --listAudioDevices`.

## 4. Enable on boot
    mkdir -p ~/.config/systemd/user
    cp systemd/dropkick.service ~/.config/systemd/user/
    systemctl --user daemon-reload
    systemctl --user enable --now dropkick.service
    # Allow the user service to run without an active login session:
    sudo loginctl enable-linger "$USER"

## 5. Phone remote
On a phone on the same LAN, open `http://<pi-ip>:8080`.
If a token is set (`remote.token` / `DROPKICK_REMOTE_TOKEN`), use
`http://<pi-ip>:8080/?token=<token>`.

## Incremental rebuild after edits
    ./scripts/build.sh

## Where configuration lives
The app reads `projectMSDL.properties` (installed to
`~/.local/share/projectMSDL/projectMSDL.properties` by `bootstrap.sh`). That
file is the source of truth for `projectM.presetPath`, `projectM.texturePath`,
`audio.device`, and the `remote.*` keys. If a key is absent the remote falls
back to safe defaults (port 8080, no token, preset/web roots under
`~/.local/share/dropkick`).

## Notes / troubleshooting
- **Wayland vs X11:** the service sets `DISPLAY=:0` for an X11 session. On a
  Wayland desktop (Raspberry Pi OS Bookworm defaults to labwc/Wayland), SDL2 may
  need `WAYLAND_DISPLAY=wayland-0` instead — set it in `dropkick.local.env` if
  the window doesn't appear under the service. Launching `projectMSDL` manually
  from the desktop session always uses the active display.
- **Remote didn't start:** check `journalctl --user -u dropkick` for a
  "failed to bind port" error (another process may hold the port).

## GLES patch rationale
See `docs/patches.md`.
