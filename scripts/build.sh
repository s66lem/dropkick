#!/usr/bin/env bash
# Incremental rebuild of both forks. Assumes bootstrap.sh already ran.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX="${DROPKICK_PREFIX:-$HOME/.local}"

cmake --build "$ROOT/external/projectm/build" -j"$(nproc)"
cmake --install "$ROOT/external/projectm/build"
cmake --build "$ROOT/external/frontend-sdl-cpp/build" -j"$(nproc)"
cmake --install "$ROOT/external/frontend-sdl-cpp/build"
echo "Build complete. Binary: $PREFIX/bin/projectMSDL"
