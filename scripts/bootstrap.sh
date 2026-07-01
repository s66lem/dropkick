#!/usr/bin/env bash
# Dropkick bootstrap: install deps, init submodules, build with GLES, install assets.
# Idempotent and re-runnable. Run from a clone of the dropkick superproject.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX="${DROPKICK_PREFIX:-$HOME/.local}"
DATA="$HOME/.local/share/dropkick"

echo "== Installing build dependencies =="
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake git pkg-config gettext-base \
  libsdl2-dev libgles2-mesa-dev libegl1-mesa-dev \
  libpoco-dev libglm-dev \
  libpipewire-0.3-dev

echo "== Initializing submodules =="
git -C "$ROOT" submodule update --init --recursive

echo "== Building libprojectM (GLES) =="
cmake -S "$ROOT/external/projectm" -B "$ROOT/external/projectm/build" \
  -DENABLE_GLES=TRUE -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX"
cmake --build "$ROOT/external/projectm/build" -j"$(nproc)"
cmake --install "$ROOT/external/projectm/build"

echo "== Building frontend (GLES) =="
cmake -S "$ROOT/external/frontend-sdl-cpp" -B "$ROOT/external/frontend-sdl-cpp/build" \
  -DENABLE_GLES=TRUE -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$PREFIX" -DCMAKE_INSTALL_PREFIX="$PREFIX"
cmake --build "$ROOT/external/frontend-sdl-cpp/build" -j"$(nproc)"
cmake --install "$ROOT/external/frontend-sdl-cpp/build"

echo "== Installing config, remote assets, and preset/texture dirs =="
mkdir -p "$DATA/presets/cream-of-the-crop" "$DATA/textures" "$DATA/remote"
cp -f "$ROOT/remote/"* "$DATA/remote/"
# Runtime env file sourced by the systemd unit (do not clobber an existing edited copy).
if [ ! -f "$DATA/dropkick.env" ]; then
  install -Dm644 "$ROOT/config/dropkick.env" "$DATA/dropkick.env"
fi
# Render projectMSDL.properties from dropkick.env (the config source of truth).
"$ROOT/scripts/sync-config.sh"

echo "== Done. =="
echo "Add presets to $DATA/presets/<pack>/ and textures to $DATA/textures/."
echo "Start: $PREFIX/bin/projectMSDL   |   Remote: http://<pi-ip>:${DROPKICK_REMOTE_PORT:-8080}"
