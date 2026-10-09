#!/usr/bin/env bash
# pifx installer for Raspberry Pi OS (Bookworm / Trixie) and other Debian/Ubuntu boxes.
# Safe to re-run.
#
#   ./install.sh                  packages + DAC overlay + release build
#   ./install.sh --service        ... and run `pifx headless` at boot (systemd user service)
#   ./install.sh --autostart      ... and open the GUI when the desktop session starts
#   ./install.sh --debug          debug build (for gdb / VS Code)
#   ./install.sh --overlay hifiberry-dacplus     a different DAC overlay
#   ./install.sh --no-overlay     leave config.txt alone (other DACs, not a Pi)
#   ./install.sh --no-apt         skip apt (packages already there)
#   ./install.sh --dry-run        show what would change
set -euo pipefail

OVERLAY="allo-boss-dac-pcm512x-audio"   # InnoMaker HiFi DAC HAT (PCM5122 as clock master)
DO_OVERLAY=1 SERVICE=0 AUTOSTART=0 DRY=0 APT=1 BUILD_TYPE=Release
while [ $# -gt 0 ]; do
  case "$1" in
    --overlay) OVERLAY="$2"; shift ;;
    --no-overlay) DO_OVERLAY=0 ;;
    --service) SERVICE=1 ;;
    --autostart) AUTOSTART=1 ;;
    --debug) BUILD_TYPE=Debug ;;
    --dry-run) DRY=1 ;;
    --no-apt) APT=0 ;;
    -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done

HERE="$(cd "$(dirname "$0")" && pwd)"
RUN_USER="${SUDO_USER:-${USER:-$(id -un)}}"
run() { echo "+ $*"; if [ "$DRY" = 0 ]; then "$@"; fi; }
CHANGED=0

echo "== pifx install  (dir: $HERE, user: $RUN_USER, build: $BUILD_TYPE)"

# ---- 1. board ------------------------------------------------------------------
MODEL="$( { tr -d '\0' < /proc/device-tree/model; } 2>/dev/null || true)"
echo "board: ${MODEL:-not a Raspberry Pi}"
[ -z "$MODEL" ] && DO_OVERLAY=0

# ---- 2. packages ---------------------------------------------------------------
if [ "$APT" = 1 ]; then
  PKGS="build-essential cmake ninja-build git pkg-config libsdl2-dev libgles-dev libegl-dev alsa-utils pulseaudio-utils"
  run sudo apt-get update
  # shellcheck disable=SC2086
  run sudo apt-get install -y $PKGS
fi

# ---- 3. DAC overlay in config.txt ------------------------------------------------
if [ "$DO_OVERLAY" = 1 ]; then
  CONFIG=/boot/firmware/config.txt
  [ -f "$CONFIG" ] || CONFIG=/boot/config.txt
  DAC_RE='(allo-boss-dac-pcm512x-audio|hifiberry-dac[a-z-]*|iqaudio-dac[a-z-]*|justboom-dac|allo-piano-dac[a-z-]*)'
  if grep -Eq "^\s*dtoverlay\s*=\s*${OVERLAY}(,|\s*$)" "$CONFIG"; then
    echo "overlay already present: dtoverlay=$OVERLAY"
  elif grep -Eq "^\s*dtoverlay\s*=\s*${DAC_RE}" "$CONFIG"; then
    echo "replacing the existing DAC overlay with dtoverlay=$OVERLAY"
    [ "$DRY" = 0 ] && sudo sed -i -E "s~^\s*dtoverlay\s*=\s*${DAC_RE}[^#]*~dtoverlay=${OVERLAY} ~" "$CONFIG"
    CHANGED=1
  else
    echo "adding dtoverlay=$OVERLAY to $CONFIG"
    [ "$DRY" = 0 ] && printf '\n# pifx: PCM5122 DAC HAT (InnoMaker HiFi DAC). Alternative: hifiberry-dacplus\ndtoverlay=%s\n' "$OVERLAY" \
      | sudo tee -a "$CONFIG" >/dev/null
    CHANGED=1
  fi
fi
if ! id -nG "$RUN_USER" | tr ' ' '\n' | grep -qx audio; then
  run sudo usermod -aG audio "$RUN_USER"
fi

# ---- 4. build ---------------------------------------------------------------------
NATIVE=OFF
[ -n "$MODEL" ] && NATIVE=ON
run cmake -S "$HERE" -B "$HERE/build" -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DPIFX_NATIVE="$NATIVE"
run cmake --build "$HERE/build" -j"$(nproc)"
run "$HERE/build/pifx_tests"
if [ ! -e "$HERE/.vscode" ]; then                # F5 = build + debug under gdb in VS Code
  run cp -r "$HERE/packaging/vscode" "$HERE/.vscode"
fi
run mkdir -p "$HOME/.local/bin"
run ln -sf "$HERE/build/pifx" "$HOME/.local/bin/pifx"

# ---- 5. start at boot ---------------------------------------------------------------
if [ "$SERVICE" = 1 ]; then
  UNIT_DIR="$HOME/.config/systemd/user"
  echo "installing $UNIT_DIR/pifx.service (runs as $RUN_USER so it can reach PipeWire)"
  if [ "$DRY" = 0 ]; then
    mkdir -p "$UNIT_DIR"
    sed -e "s|__DIR__|$HERE|g" "$HERE/packaging/pifx.service" > "$UNIT_DIR/pifx.service"
    systemctl --user daemon-reload
    systemctl --user enable --now pifx.service
    sudo loginctl enable-linger "$RUN_USER"     # start at boot, before anyone logs in
  fi
fi
if [ "$AUTOSTART" = 1 ]; then
  echo "installing ~/.config/autostart/pifx.desktop"
  if [ "$DRY" = 0 ]; then
    mkdir -p "$HOME/.config/autostart" "$HOME/.local/share/applications"
    sed -e "s|__DIR__|$HERE|g" "$HERE/packaging/pifx.desktop" > "$HOME/.config/autostart/pifx.desktop"
    cp "$HOME/.config/autostart/pifx.desktop" "$HOME/.local/share/applications/pifx.desktop"
  fi
fi

echo
echo "== done"
[ "$CHANGED" = 1 ] && echo "config.txt changed: reboot, then check the HAT with:   pifx diag"
[ "$CHANGED" = 0 ] && echo "check the HAT with:   pifx diag"
echo "run:                 pifx            (GUI)      pifx headless      pifx --help"
[ "$SERVICE" = 1 ] && echo "service log:         journalctl --user -u pifx -f"
case ":$PATH:" in *":$HOME/.local/bin:"*) ;; *) echo "note: add ~/.local/bin to PATH, or run $HERE/build/pifx" ;; esac
exit 0
