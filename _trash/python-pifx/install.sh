#!/usr/bin/env bash
# pifx installer for Raspberry Pi OS (Bookworm / Trixie, 32- or 64-bit). Safe to re-run.
#
#   ./install.sh                 install packages, enable the DAC overlay
#   ./install.sh --service       ... and run pifx at boot as a systemd service
#   ./install.sh --overlay hifiberry-dacplus     use a different overlay
#   ./install.sh --dry-run       show what would change
#
set -euo pipefail

OVERLAY="allo-boss-dac-pcm512x-audio"   # InnoMaker HiFi DAC HAT (PCM5122 as clock master)
SERVICE=0
DRY=0
APT=1
while [ $# -gt 0 ]; do
  case "$1" in
    --overlay) OVERLAY="$2"; shift ;;
    --service) SERVICE=1 ;;
    --dry-run) DRY=1 ;;
    --no-apt) APT=0 ;;
    -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done

HERE="$(cd "$(dirname "$0")" && pwd)"
RUN_USER="${SUDO_USER:-${USER:-$(id -un)}}"
run() { if [ "$DRY" = 1 ]; then echo "+ $*"; else echo "+ $*"; "$@"; fi; }

echo "== pifx install  (dir: $HERE, user: $RUN_USER)"

# ---- 1. are we on a Pi? ----------------------------------------------------
MODEL="$( { tr -d '\0' < /proc/device-tree/model; } 2>/dev/null || true)"
if [ -z "$MODEL" ]; then
  echo "This is not a Raspberry Pi. Skipping the overlay; you can still run: python3 -m pifx --sim"
fi
echo "board: ${MODEL:-unknown}"

# ---- 2. packages -------------------------------------------------------------
if [ "$APT" = 1 ]; then
  PKGS="python3 python3-numpy python3-scipy python3-sounddevice python3-mido python3-rtmidi alsa-utils libportaudio2"
  run sudo apt-get update
  run sudo apt-get install -y $PKGS
fi

# ---- 3. DAC overlay in config.txt -------------------------------------------
if [ -n "$MODEL" ]; then
  CONFIG=/boot/firmware/config.txt
  [ -f "$CONFIG" ] || CONFIG=/boot/config.txt
  CHANGED=0
  if grep -Eq "^\s*dtoverlay\s*=\s*${OVERLAY}(,|\s*$)" "$CONFIG"; then
    echo "overlay already present in $CONFIG: dtoverlay=$OVERLAY"
  elif grep -Eq "^\s*dtoverlay\s*=\s*(allo-boss-dac-pcm512x-audio|hifiberry-dac|hifiberry-dacplus|iqaudio-dac|justboom-dac|allo-piano-dac)" "$CONFIG"; then
    CUR="$(grep -E "^\s*dtoverlay\s*=\s*(allo-boss|hifiberry-dac|iqaudio-dac|justboom-dac|allo-piano-dac)" "$CONFIG" | head -1)"
    echo "another DAC overlay is configured: $CUR"
    echo "replacing it with dtoverlay=$OVERLAY"
    if [ "$DRY" = 0 ]; then
      sudo sed -i -E "s~^\s*dtoverlay\s*=\s*(allo-boss-dac-pcm512x-audio|hifiberry-dac[a-z-]*|iqaudio-dac[a-z-]*|justboom-dac|allo-piano-dac[a-z-]*)[^#]*~dtoverlay=${OVERLAY} ~" "$CONFIG"
    fi
    CHANGED=1
  else
    echo "adding dtoverlay=$OVERLAY to $CONFIG"
    if [ "$DRY" = 0 ]; then
      printf '\n# pifx: PCM5122 DAC HAT (InnoMaker HiFi DAC). Alternative: hifiberry-dacplus\ndtoverlay=%s\n' "$OVERLAY" | sudo tee -a "$CONFIG" >/dev/null
    fi
    CHANGED=1
  fi
  # audio group for direct ALSA access
  if ! id -nG "$RUN_USER" | tr ' ' '\n' | grep -qx audio; then
    run sudo usermod -aG audio "$RUN_USER"
  fi
fi

# ---- 4. optional systemd service ---------------------------------------------
if [ "$SERVICE" = 1 ]; then
  UNIT=/etc/systemd/system/pifx.service
  echo "installing $UNIT"
  if [ "$DRY" = 0 ]; then
    sed -e "s|__DIR__|$HERE|g" -e "s|__USER__|$RUN_USER|g" "$HERE/pifx.service" | sudo tee "$UNIT" >/dev/null
    sudo systemctl daemon-reload
    sudo systemctl enable pifx.service
    if [ "${CHANGED:-0}" = 0 ]; then sudo systemctl restart pifx.service; fi
  fi
fi

echo
echo "== done"
if [ "${CHANGED:-0}" = 1 ]; then
  echo "config.txt changed: reboot now, then check the HAT with:   python3 -m pifx diag"
else
  echo "check the HAT with:   python3 -m pifx diag"
fi
echo "run the box with:     python3 -m pifx          (web UI on port 8080)"
[ "$SERVICE" = 1 ] && echo "service log:          journalctl -u pifx -f"
exit 0
