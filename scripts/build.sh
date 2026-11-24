#!/usr/bin/env bash
# Incremental rebuild of both forks. Assumes bootstrap.sh already ran.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DATA="$HOME/.local/share/dropkick"
# Same prefix precedence as bootstrap.sh: caller env, then the installed dropkick.env.
PREFIX="${DROPKICK_PREFIX:-}"
if [ -z "$PREFIX" ] && [ -f "$DATA/dropkick.env" ]; then
  PREFIX="$(. "$DATA/dropkick.env" >/dev/null 2>&1; echo "${DROPKICK_PREFIX:-}")"
fi
PREFIX="${PREFIX:-$HOME/.local}"

cmake --build "$ROOT/external/projectm/build" -j"$(nproc)"
cmake --install "$ROOT/external/projectm/build"
cmake --build "$ROOT/external/frontend-sdl-cpp/build" -j"$(nproc)"
cmake --install "$ROOT/external/frontend-sdl-cpp/build"
# Deploy remote web UI edits too (bootstrap.sh does the initial copy).
mkdir -p "$DATA/remote"
cp -f "$ROOT/remote/"* "$DATA/remote/"
echo "Build complete. Binary: $PREFIX/bin/projectMSDL"
