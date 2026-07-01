#!/usr/bin/env bash
# Render projectMSDL.properties from dropkick.env. Run after editing the env file
# (then restart the service). Requires `envsubst` (gettext-base).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DATA="$HOME/.local/share/dropkick"

# Load config values: repo defaults (baseline so every var is defined), then the
# installed env, then an optional local override.
set -a
. "$ROOT/config/dropkick.env"
[ -f "$DATA/dropkick.env" ] && . "$DATA/dropkick.env"
[ -f "$DATA/dropkick.local.env" ] && . "$DATA/dropkick.local.env"
set +a

PREFIX="${DROPKICK_PREFIX:-$HOME/.local}"
DEST="$PREFIX/share/projectMSDL/projectMSDL.properties"

# Only substitute the DROPKICK_* placeholders; leave any Poco ${...} intact.
VARS='$DROPKICK_PRESET_ROOT $DROPKICK_PRESET_PACK $DROPKICK_TEXTURE_ROOT $DROPKICK_AUDIO_SOURCE $DROPKICK_REMOTE_PORT $DROPKICK_REMOTE_TOKEN $DROPKICK_WEB_ROOT'

mkdir -p "$(dirname "$DEST")"
envsubst "$VARS" < "$ROOT/config/projectMSDL.properties.in" > "$DEST"
echo "Rendered $DEST from dropkick.env"
