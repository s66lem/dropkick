#!/usr/bin/env bash
# Seed the GPU-hang blocklist with a static heuristic: flag presets whose warp/
# composite shaders are heavy enough to risk hanging the Pi's V3D GPU. This is a
# STARTING POINT, not ground truth — the runtime auto-skip refines it, and you can
# undo it anytime with the remote's "Clear" (Settings → Blocked presets).
#
# Usage: seed-blocklist.sh [--dry-run] [shader_byte_threshold]
#   --dry-run   list candidates, don't write the blocklist
#   threshold   warp+comp shader bytes above which a preset is flagged (default 6000)
#
# Only warp_N= / comp_N= lines are inspected, with the key and `//` comments
# removed. One awk pass per batch of files (no per-file forks). Portable across
# mawk (Raspberry Pi OS default) and gawk: files are flushed on FNR==1 and in END,
# not ENDFILE, and `\b` is avoided.
set -euo pipefail
export LC_ALL=C # byte-wise length() and character classes, regardless of locale

DRY=0
if [ "${1:-}" = "--dry-run" ]; then DRY=1; shift; fi
THRESH="${1:-6000}"
case "$THRESH" in
  ''|*[!0-9]*) echo "threshold must be a non-negative integer, got: $THRESH" >&2; exit 2 ;;
esac

DATA="$HOME/.local/share/dropkick"
ROOT="${DROPKICK_PRESET_ROOT:-$DATA/presets}"
BLOCKLIST="$DATA/blocklist.txt"
mkdir -p "$DATA"

# Absolute path: every file handed to awk then starts with "/", so a name that
# contains "=" can never be taken for a var=value assignment.
if [ ! -d "$ROOT" ]; then echo "Preset root not found: $ROOT" >&2; exit 1; fi
ROOT="$(cd "$ROOT" && pwd)"

echo "Scanning presets under: $ROOT"
echo "Flagging: shader bytes > $THRESH, or >=3 GetBlur taps, or shader loops."

# Per-file stats -> tab-separated records on stdout:
#   F<TAB>path   preset matches the heuristic
#   A<TAB>path   ...and is not already in the blocklist
#   S<TAB>n      files scanned by this awk invocation (find may run several)
# Empty files emit no lines, so they are not counted (and can't match anyway).
AWK_PROG='
function flush() {
  if (cur == "") return
  n++
  if (bytes > T || blur >= 3 || loops >= 1) {
    print "F\t" cur
    if (!(cur in seen)) print "A\t" cur
  }
}
BEGIN {
  while ((getline line < BL) > 0) if (line != "") seen[line] = 1
  close(BL)
}
FNR == 1 { flush(); cur = FILENAME; bytes = 0; blur = 0; loops = 0 }
/^(warp|comp)_[0-9]+=/ {
  body = $0
  sub(/\r$/, "", body)
  sub(/^(warp|comp)_[0-9]+=/, "", body)
  sub(/\/\/.*/, "", body)
  bytes += length(body) + 1
  s = body
  # GetBlur1..3 as whole identifiers; the trailing delimiter is kept for the next match.
  while (match(s, /(^|[^A-Za-z0-9_])GetBlur[0-9]?([^A-Za-z0-9_]|$)/)) {
    blur++
    s = substr(s, RSTART + RLENGTH - 1)
  }
  if (body ~ /(^|[^A-Za-z0-9_])for[ \t]*\(/) loops++
}
END { flush(); print "S\t" n + 0 }
'

out="$(mktemp)"
tmp="$(mktemp)"
trap 'rm -f "$out" "$tmp"' EXIT

# A failing find or awk aborts the script (set -e) instead of reporting a partial scan.
find -L "$ROOT" -type f -iname '*.milk' \
  -exec awk -v T="$THRESH" -v BL="$BLOCKLIST" "$AWK_PROG" {} + > "$out"

total=$(awk -F'\t' '$1 == "S" { s += $2 } END { print s + 0 }' "$out")
flagged=$(awk -F'\t' '$1 == "F" { c++ } END { print c + 0 }' "$out")
awk '/^A\t/ { sub(/^A\t/, ""); print }' "$out" > "$tmp"
newly=$(wc -l < "$tmp" | tr -d ' ')

echo "Scanned $total presets; $flagged match the heuristic ($newly new)."

if [ "$DRY" = "1" ]; then
  echo "--- dry run: candidates (not written) ---"
  sort "$tmp" | sed -n '1,40p'
  echo "(showing up to 40; rerun without --dry-run to apply)"
  exit 0
fi

# Append new entries to the blocklist.
if [ "$newly" -gt 0 ]; then
  cat "$tmp" >> "$BLOCKLIST"
  echo "Added $newly presets to $BLOCKLIST (now $(wc -l < "$BLOCKLIST") total)."
else
  echo "No new presets to add."
fi
echo "Restart Dropkick to apply. Undo anytime via the remote's Clear."
