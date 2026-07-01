# Dropkick

A reproducible projectM audio visualizer for the Raspberry Pi 5 with GLES 3.1
support, preset-pack management, and phone remote control.

See `docs/setup.md` for build and install instructions.

## Layout
- `external/` — patched forks (submodules): libprojectM + projectMSDL frontend
- `patches/` — exported documentation patches
- `remote/` — mobile remote web page
- `scripts/` — bootstrap & build
- `config/` — runtime configuration
- `systemd/` — boot service
