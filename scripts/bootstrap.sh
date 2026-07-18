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

echo "== Building libprojectM (GLES) =="
cmake -S "$ROOT/external/projectm" -B "$ROOT/external/projectm/build" \
  -DENABLE_GLES=TRUE -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_INSTALL_RPATH="$PREFIX/lib" -DCMAKE_INSTALL_RPATH_USE_LINK_PATH=TRUE
cmake --build "$ROOT/external/projectm/build" -j"$(nproc)"
cmake --install "$ROOT/external/projectm/build"

echo "== Building frontend (GLES) =="
# RPATH so projectMSDL finds libprojectM under $PREFIX/lib without LD_LIBRARY_PATH.
cmake -S "$ROOT/external/frontend-sdl-cpp" -B "$ROOT/external/frontend-sdl-cpp/build" \
  -DENABLE_GLES=TRUE -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$PREFIX" -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_INSTALL_RPATH="$PREFIX/lib" -DCMAKE_INSTALL_RPATH_USE_LINK_PATH=TRUE
cmake --build "$ROOT/external/frontend-sdl-cpp/build" -j"$(nproc)"
cmake --install "$ROOT/external/frontend-sdl-cpp/build"

echo "== Installing config, remote assets, and preset/texture dirs =="
mkdir -p "$DATA/presets/cream-of-the-crop" "$DATA/textures" "$DATA/remote" "$DATA/workshop" "$DATA/state"
cp -f "$ROOT/remote/"* "$DATA/remote/"
# Seed a starter preset for the workshop (don't clobber the user's edits).
if [ ! -f "$DATA/workshop/starter.milk" ]; then
  install -Dm644 "$ROOT/share/starter.milk" "$DATA/workshop/starter.milk"
fi
# Runtime env file sourced by the systemd unit (do not clobber an existing edited copy).
if [ ! -f "$DATA/dropkick.env" ]; then
  install -Dm644 "$ROOT/config/dropkick.env" "$DATA/dropkick.env"
  # Short, typeable token: the remote lives at http://<pi>:8080/op1. Bump the
  # number (op2, op3, ...) in dropkick.env to rotate it, or blank
  # DROPKICK_REMOTE_TOKEN for an open remote.
  token="op1"
  sed -i "s#^DROPKICK_REMOTE_TOKEN=.*#DROPKICK_REMOTE_TOKEN=\"$token\"#" "$DATA/dropkick.env"
  echo "Remote-control token: $token (see DROPKICK_REMOTE_TOKEN in $DATA/dropkick.env)."
fi
# Render projectMSDL.properties from dropkick.env (the config source of truth).
"$ROOT/scripts/sync-config.sh"

# Install the launcher wrapper (sets LD_LIBRARY_PATH + display env, sources env).
install -Dm755 "$ROOT/scripts/dropkick-launcher.sh" "$PREFIX/bin/dropkick"

# Install the desktop menu entry (appears under Sound & Video). Reuses the
# projectMSDL icon installed by the frontend build.
mkdir -p "$HOME/.local/share/applications"
sed "s#__DROPKICK_BIN__#$PREFIX/bin/dropkick#g" "$ROOT/share/dropkick.desktop.in" \
  > "$HOME/.local/share/applications/dropkick.desktop"
update-desktop-database "$HOME/.local/share/applications" 2>/dev/null || true

echo "== Done. =="
echo "Add presets to $DATA/presets/<pack>/ and textures to $DATA/textures/."
# Read back the effective port/token for the remote URL hint.
remote_port="$(sed -n 's#^DROPKICK_REMOTE_PORT="\{0,1\}\([^"]*\)"\{0,1\}#\1#p' "$DATA/dropkick.env")"
remote_token="$(sed -n 's#^DROPKICK_REMOTE_TOKEN="\{0,1\}\([^"]*\)"\{0,1\}#\1#p' "$DATA/dropkick.env")"
remote_url="http://<pi-ip>:${remote_port:-8080}"
[ -n "$remote_token" ] && remote_url="$remote_url/$remote_token"
echo "Start: dropkick    (or $PREFIX/bin/dropkick)   |   Remote: $remote_url"
