#!/usr/bin/env bash
# Enable the hardware watchdog (auto-reboot if the Pi hard-locks, e.g. a GPU hang)
# and set Dropkick to start on boot. Run this on the Pi as your normal user:
#
#   bash ~/dropkick/scripts/enable-watchdog.sh
#
# It uses sudo for the system-level bits (watchdog config + linger) and will
# prompt for your password. Safe to re-run.
set -eu
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "== 1/3 Hardware watchdog (auto-reboot on hard lockup) =="
sudo mkdir -p /etc/systemd/system.conf.d
sudo tee /etc/systemd/system.conf.d/dropkick-watchdog.conf >/dev/null <<'EOF'
[Manager]
# Reset the board if systemd stops petting the watchdog (hard lockup).
RuntimeWatchdogSec=15
RebootWatchdogSec=2min
EOF
sudo systemctl daemon-reexec
echo "   watchdog set (RuntimeWatchdogSec=15). Verify with: systemctl show -p RuntimeWatchdogUSec"

echo "== 2/3 Start Dropkick on boot (user service) =="
mkdir -p "$HOME/.config/systemd/user"
cp -f "$ROOT/systemd/dropkick.service" "$HOME/.config/systemd/user/dropkick.service"
systemctl --user daemon-reload
systemctl --user enable dropkick.service

echo "== 3/3 Allow the user service to run without an active login (linger) =="
sudo loginctl enable-linger "$USER"

echo
echo "Done. The Pi will now auto-reboot on a hard lockup and relaunch Dropkick on boot."
echo "To run the accurate blocklist scan next, see docs/setup.md (set a short preset"
echo "duration from the remote and let it cycle; hard-hangers get quarantined across reboots)."
