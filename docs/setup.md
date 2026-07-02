# Dropkick Setup (Raspberry Pi 5)

## 1. Clone and bootstrap
    git clone https://github.com/s66lem/dropkick.git
    cd dropkick
    ./scripts/bootstrap.sh

Everything is vendored in one repo (no submodules). This installs dependencies,
builds the bundled libprojectM + frontend with `-DENABLE_GLES=TRUE`, and
installs the binary, remote web assets, config, and preset/texture dirs. It also
drops a `dropkick.env` into `~/.local/share/dropkick/` (only if one isn't there).

## 2. Add presets & textures
Drop preset packs into subfolders:

    ~/.local/share/dropkick/presets/<pack-name>/*.milk
    ~/.local/share/dropkick/textures/

## 3. Configure
`~/.local/share/dropkick/dropkick.env` is the single source of truth. Edit it
(audio device, preset pack, remote port/token), then render the app config and
restart:

    ./scripts/sync-config.sh
    systemctl --user restart dropkick.service

Leave `DROPKICK_AUDIO_SOURCE` empty for the default device; list devices with
`projectMSDL --listAudioDevices`.

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

## Launching
`bootstrap.sh` installs a `dropkick` launcher on your PATH. Just run:

    dropkick

It sets `LD_LIBRARY_PATH` (libprojectM lives under `~/.local/lib`) and sensible
display defaults, then execs `projectMSDL`.

It also installs a **Dropkick** menu entry under **Sound & Video** (the desktop
menu), so you can launch it like the stock projectM. The entry runs the same
wrapper.

## Where configuration lives
You configure Dropkick in **`dropkick.env`**. `bootstrap.sh` and
`sync-config.sh` render that into `~/.local/projectMSDL.properties` via
`envsubst` — this is the file the app actually reads (Poco's default config
path, next to the prefix, NOT under `share/`). Never edit the rendered
`.properties` directly; edit `dropkick.env` and re-run `sync-config.sh`. If a
`remote.*` key is somehow absent, the remote falls back to safe defaults (port
8080, no token, preset/web roots under `~/.local/share/dropkick`).

## Notes / troubleshooting
- **Wayland vs X11:** the service sets `DISPLAY=:0` for an X11 session. On a
  Wayland desktop (Raspberry Pi OS Bookworm defaults to labwc/Wayland), SDL2 may
  need `WAYLAND_DISPLAY=wayland-0` instead — set it in `dropkick.local.env` if
  the window doesn't appear under the service. Launching `projectMSDL` manually
  from the desktop session always uses the active display.
- **Remote didn't start:** check `journalctl --user -u dropkick` for a
  "failed to bind port" error (another process may hold the port).

## Robustness: watchdog + blocklist scan
Some shader-heavy presets can hard-hang the Pi's GPU (locks up the whole board).
To recover automatically and learn which presets to avoid:

    bash ~/dropkick/scripts/enable-watchdog.sh   # sudo: auto-reboot on lockup + start on boot

Then run the accurate scan: from the remote's Settings, set **Preset duration** low
(e.g. 3s) so it cycles quickly. When it hits a hard-hanger the watchdog reboots the
Pi, the offending preset is quarantined on the next boot, and it resumes — over a few
hours the blocklist self-populates. Set the duration back up when the blocked count
stops climbing. Undo the blocklist anytime via Settings → Blocked presets → Clear.

## GLES patch rationale
See `docs/patches.md`.
