#!/usr/bin/env bash
# Dropkick launcher — installed as ~/.local/bin/dropkick by bootstrap.sh.
# Sources the runtime env, ensures the libprojectM libraries are on the loader
# path (belt-and-suspenders alongside the RPATH baked in at build time), and
# provides sensible display defaults for the Pi's graphical session.
DATA="$HOME/.local/share/dropkick"
if [ -f "$DATA/dropkick.env" ]; then
  set -a
  . "$DATA/dropkick.env"
  [ -f "$DATA/dropkick.local.env" ] && . "$DATA/dropkick.local.env"
  set +a
fi
PREFIX="${DROPKICK_PREFIX:-$HOME/.local}"
export LD_LIBRARY_PATH="$PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export DISPLAY="${DISPLAY:-:0}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
exec "$PREFIX/bin/projectMSDL" "$@"
