#!/usr/bin/env bash
# Seed the GPU-hang blocklist with a static heuristic: flag presets whose warp/
# composite shaders are heavy enough to risk hanging the Pi's V3D GPU. This is a
# STARTING POINT, not ground truth — the runtime auto-skip refines it, and you can
# undo it anytime with the remote's "Clear" (Settings → Blocked presets).
#
# Usage: seed-blocklist.sh [--dry-run] [shader_byte_threshold]
#   --dry-run   list candidates, don't write the blocklist
#   threshold   warp+comp shader bytes above which a preset is flagged (default 6000)
# Note: no `pipefail` — grep exits 1 on "no match", which is normal here.
set -eu

DRY=0
[ "${1:-}" = "--dry-run" ] && { DRY=1; shift; }
THRESH="${1:-6000}"

DATA="$HOME/.local/share/dropkick"
ROOT="${DROPKICK_PRESET_ROOT:-$DATA/presets}"
BLOCKLIST="$DATA/blocklist.txt"
mkdir -p "$DATA"

echo "Scanning presets under: $ROOT"
echo "Flagging: shader bytes > $THRESH, or >=3 GetBlur taps, or shader loops."

# Existing blocklist entries (avoid duplicates).
declare -A seen
if [ -f "$BLOCKLIST" ]; then
  while IFS= read -r line; do [ -n "$line" ] && seen["$line"]=1; done < "$BLOCKLIST"
fi

total=0
flagged=0
newly=0
tmp="$(mktemp)"

while IFS= read -r -d '' f; do
  total=$((total + 1))
  # Bytes in warp_/comp_ shader lines (the pixel-shader body).
  shader_bytes=$(grep -aE '^(warp|comp)_[0-9]+=' "$f" 2>/dev/null | wc -c)
  blur=$(grep -aoE 'GetBlur[0-9]?' "$f" 2>/dev/null | wc -l)
  loops=$(grep -acE '\bfor[[:space:]]*\(' "$f" 2>/dev/null || true)
  if [ "$shader_bytes" -gt "$THRESH" ] || [ "$blur" -ge 3 ] || [ "${loops:-0}" -ge 1 ]; then
    flagged=$((flagged + 1))
    if [ -z "${seen[$f]:-}" ]; then
      echo "$f" >> "$tmp"
      newly=$((newly + 1))
    fi
  fi
done < <(find -L "$ROOT" -type f -iname '*.milk' -print0)

echo "Scanned $total presets; $flagged match the heuristic ($newly new)."

if [ "$DRY" = "1" ]; then
  echo "--- dry run: candidates (not written) ---"
  sort "$tmp" | head -40
  echo "(showing up to 40; rerun without --dry-run to apply)"
  rm -f "$tmp"
  exit 0
fi

# Append new entries to the blocklist.
if [ "$newly" -gt 0 ]; then
  cat "$tmp" >> "$BLOCKLIST"
  echo "Added $newly presets to $BLOCKLIST (now $(wc -l < "$BLOCKLIST") total)."
else
  echo "No new presets to add."
fi
rm -f "$tmp"
echo "Restart Dropkick (or switch packs) to apply. Undo anytime via the remote's Clear."
