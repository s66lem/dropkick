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

# Supervisor loop: a preset that hangs the V3D GPU can kill the app. On a failure
# exit the app quarantines the offending preset on its next start (see
# ProjectMWrapper), so we relaunch. A clean exit (0 = user quit) stops the loop.
# Rate guard: bail if it fails >=5 times within 60s to avoid a fast crash loop.
fails=0
window_start=$(date +%s)
while true; do
  "$PREFIX/bin/projectMSDL" "$@"
  code=$?
  [ "$code" -eq 0 ] && break

  now=$(date +%s)
  if [ $(( now - window_start )) -gt 60 ]; then fails=0; window_start=$now; fi
  fails=$(( fails + 1 ))
  echo "dropkick: projectMSDL exited with code $code (restart $fails)" >&2
  if [ "$fails" -ge 5 ]; then
    echo "dropkick: too many failures in 60s — giving up. Check the blocklist/logs." >&2
    exit "$code"
  fi
  sleep 1
done
