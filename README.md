# Dropkick

A reproducible projectM audio visualizer for the Raspberry Pi 5 with GLES 3.1
support, preset-pack management, and phone remote control.

See `docs/setup.md` for build and install instructions.

## Install (Raspberry Pi 5)

    git clone https://github.com/s66lem/dropkick.git
    cd dropkick && ./scripts/bootstrap.sh

## Layout
- `external/` — vendored, patched libprojectM + projectMSDL frontend (single repo, no submodules)
- `patches/` — exported documentation patches
- `remote/` — mobile remote web page
- `scripts/` — bootstrap & build
- `config/` — runtime configuration
- `systemd/` — boot service
