"""HAT detection and ALSA mixer control for PCM5122-based DAC HATs.

Targets the InnoMaker "HiFi DAC HAT" (PCM5122, dual oscillators, RCA + 3.5 mm),
but works with any pcm512x board (HiFiBerry DAC+, Allo Boss, IQaudIO DAC+ ...).

Everything in here reads plain files under /proc and /sys and shells out to
``amixer``, so it has no third-party dependencies. All paths are resolved
relative to ``SYSROOT`` (default ``/``) so the detection logic can be unit
tested against a fake tree.
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Optional

# Allow tests (and curious people) to point the detector at a fake filesystem.
SYSROOT = Path(os.environ.get("PIFX_SYSROOT", "/"))

# Device-tree overlays that drive a pcm512x codec. The first one is what the
# InnoMaker board wants: the PCM5122 is the I2S clock master and runs from the
# two on-board oscillators, which is exactly the Allo Boss design.
PCM512X_OVERLAYS = [
    "allo-boss-dac-pcm512x-audio",
    "hifiberry-dacplus",
    "hifiberry-dacplus-std",
    "hifiberry-dacplus-pro",
    "hifiberry-dac",
    "allo-piano-dac-pcm512x-audio",
    "allo-piano-dac-plus-pcm512x-audio",
    "iqaudio-dacplus",
    "iqaudio-dac",
    "justboom-dac",
    "rpi-dac",
]
RECOMMENDED_OVERLAY = "allo-boss-dac-pcm512x-audio"
FALLBACK_OVERLAY = "hifiberry-dacplus"

# Strings that identify a pcm512x card in /proc/asound/cards.
PCM512X_CARD_HINTS = (
    "bossdac", "boss", "hifiberry", "pcm512", "pcm5122", "iqaudio",
    "justboom", "pianodac", "allo", "rpi-dac", "rpidac",
)

# Kernel modules that belong to a pcm512x HAT.
PCM512X_MODULES = (
    "snd_soc_pcm512x", "snd_soc_pcm512x_i2c", "snd_soc_allo_boss_dac",
    "snd_soc_hifiberry_dacplus", "snd_soc_rpi_simple_soundcard",
)


# --------------------------------------------------------------------------
# small file helpers
# --------------------------------------------------------------------------
def _p(path: str) -> Path:
    """Resolve an absolute path inside SYSROOT."""
    return SYSROOT / path.lstrip("/")


def _read_text(path: str) -> Optional[str]:
    try:
        return _p(path).read_text(errors="replace").replace("\x00", "").strip()
    except OSError:
        return None


def _exists(path: str) -> bool:
    return _p(path).exists()


# --------------------------------------------------------------------------
# data classes
# --------------------------------------------------------------------------
@dataclass
class SoundCard:
    index: int
    id: str            # short id, e.g. "BossDAC" or "sndrpihifiberry"
    driver: str        # e.g. "BossDAC" or "snd_rpi_hifiberry_dacplus"
    name: str          # long name
    is_pcm512x: bool = False
    hw_params: Optional[str] = None   # currently running params, if any


@dataclass
class HatStatus:
    pi_model: Optional[str] = None
    is_pi5: bool = False
    eeprom: dict = field(default_factory=dict)       # vendor/product/... from HAT EEPROM
    config_path: Optional[str] = None
    overlays: list = field(default_factory=list)      # all dtoverlay= lines
    audio_overlay: Optional[str] = None               # the pcm512x overlay in use
    onboard_audio: Optional[bool] = None              # dtparam=audio=on?
    modules: list = field(default_factory=list)       # loaded pcm512x modules
    i2c_codec: Optional[str] = None                   # e.g. "1-004d: pcm5122"
    cards: list = field(default_factory=list)         # all SoundCards
    card: Optional[SoundCard] = None                  # the detected HAT card
    detected: bool = False
    pipewire: bool = False
    pulseaudio: bool = False
    issues: list = field(default_factory=list)
    recommendations: list = field(default_factory=list)

    def to_dict(self) -> dict:
        d = asdict(self)
        return d

    def summary(self) -> str:
        if self.detected and self.card:
            return f"{self.card.name} (card {self.card.index}, hw:{self.card.index})"
        return "no PCM5122 DAC detected"


# --------------------------------------------------------------------------
# detection pieces
# --------------------------------------------------------------------------
def pi_model() -> Optional[str]:
    return _read_text("/proc/device-tree/model")


def read_eeprom() -> dict:
    """HAT EEPROM fields the firmware exposes in the device tree."""
    out = {}
    for key in ("vendor", "product", "product_id", "product_ver", "uuid"):
        val = _read_text(f"/proc/device-tree/hat/{key}")
        if val:
            out[key] = val
    return out


def find_config_txt() -> Optional[str]:
    # Bookworm and later (including every Pi 5 image) use /boot/firmware.
    for cand in ("/boot/firmware/config.txt", "/boot/config.txt"):
        if _exists(cand):
            return cand
    return None


def parse_config(text: str) -> tuple[list, Optional[bool]]:
    """Return (dtoverlay names, dtparam audio on/off/None) from config.txt."""
    overlays: list[str] = []
    audio: Optional[bool] = None
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        m = re.match(r"dtoverlay\s*=\s*(.+)", line)
        if m:
            overlays.append(m.group(1).strip())
            continue
        m = re.match(r"dtparam\s*=\s*(.+)", line)
        if m:
            for part in m.group(1).split(","):
                k, _, v = part.strip().partition("=")
                if k.strip() == "audio":
                    audio = v.strip().lower() in ("", "on", "1", "true", "yes")
    return overlays, audio


def read_config() -> tuple[Optional[str], list, Optional[bool]]:
    path = find_config_txt()
    if not path:
        return None, [], None
    text = _read_text(path) or ""
    overlays, audio = parse_config(text)
    return path, overlays, audio


def pcm512x_overlay(overlays: list) -> Optional[str]:
    for ov in overlays:
        base = ov.split(",", 1)[0].strip()
        if base in PCM512X_OVERLAYS:
            return ov
    return None


def loaded_modules() -> list:
    text = _read_text("/proc/modules") or ""
    names = {line.split()[0] for line in text.splitlines() if line.strip()}
    return sorted(n for n in names if n in PCM512X_MODULES)


def i2c_codec() -> Optional[str]:
    """Look for the PCM5122 on the I2C bus (normally address 0x4d on bus 1)."""
    base = _p("/sys/bus/i2c/devices")
    try:
        for dev in sorted(base.iterdir()):
            try:
                name = (dev / "name").read_text(errors="replace").strip()
            except OSError:
                continue
            if "pcm512" in name.lower():
                return f"{dev.name}: {name}"
    except OSError:
        pass
    return None


def parse_asound_cards(text: str) -> list:
    """Parse /proc/asound/cards."""
    cards: list[SoundCard] = []
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        m = re.match(r"^\s*(\d+)\s+\[(\S+)\s*\]:\s+(\S+)\s+-\s+(.*)$", lines[i])
        if m:
            idx = int(m.group(1))
            long_name = lines[i + 1].strip() if i + 1 < len(lines) else m.group(4).strip()
            card = SoundCard(index=idx, id=m.group(2), driver=m.group(3), name=long_name)
            blob = f"{card.id} {card.driver} {card.name}".lower()
            card.is_pcm512x = any(h in blob for h in PCM512X_CARD_HINTS)
            cards.append(card)
            i += 2
        else:
            i += 1
    return cards


def sound_cards() -> list:
    text = _read_text("/proc/asound/cards") or ""
    cards = parse_asound_cards(text)
    for c in cards:
        hp = _read_text(f"/proc/asound/card{c.index}/pcm0p/sub0/hw_params")
        if hp and hp != "closed":
            c.hw_params = " ".join(hp.split())
    return cards


def _running(name: str) -> bool:
    if not shutil.which("pgrep"):
        return False
    try:
        r = subprocess.run(["pgrep", "-x", name], capture_output=True, timeout=2)
        return r.returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


# --------------------------------------------------------------------------
# the full report
# --------------------------------------------------------------------------
def detect() -> HatStatus:
    st = HatStatus()
    st.pi_model = pi_model()
    st.is_pi5 = bool(st.pi_model and "Raspberry Pi 5" in st.pi_model)
    st.eeprom = read_eeprom()
    st.config_path, st.overlays, st.onboard_audio = read_config()
    st.audio_overlay = pcm512x_overlay(st.overlays)
    st.modules = loaded_modules()
    st.i2c_codec = i2c_codec()
    st.cards = sound_cards()
    st.pipewire = _running("pipewire")
    st.pulseaudio = _running("pulseaudio")

    pcm_cards = [c for c in st.cards if c.is_pcm512x]
    if pcm_cards:
        st.card = pcm_cards[0]
        st.detected = True

    cfg = st.config_path or "/boot/firmware/config.txt"

    if not st.detected:
        if st.pi_model is None:
            st.issues.append("Not running on a Raspberry Pi (no /proc/device-tree/model). "
                             "Use --sim to run without the HAT.")
        elif not st.audio_overlay:
            st.issues.append("No PCM5122 sound card and no DAC overlay configured.")
            st.recommendations.append(
                f"Add this line to {cfg} and reboot:\n    dtoverlay={RECOMMENDED_OVERLAY}\n"
                f"  (If the card still does not appear, try dtoverlay={FALLBACK_OVERLAY} instead.)")
        else:
            st.issues.append(f"Overlay '{st.audio_overlay}' is configured but no PCM5122 card appeared.")
            if not st.i2c_codec:
                st.recommendations.append(
                    "The codec was not found on I2C. Check that the HAT is seated on all 40 pins, then run\n"
                    "    sudo i2cdetect -y 1\n  and look for a device at 0x4d (or 0x4c).")
            st.recommendations.append(
                f"Try the other overlay: dtoverlay={FALLBACK_OVERLAY if st.audio_overlay.startswith('allo') else RECOMMENDED_OVERLAY}")
            if st.is_pi5:
                st.recommendations.append(
                    "On a Pi 5, if the card appears but stays silent or playback stalls, append ',slave' "
                    "to the overlay line so the Pi generates the I2S clocks.")
    else:
        if st.eeprom.get("product"):
            pass  # fine, plug and play via EEPROM
        if (st.pipewire or st.pulseaudio):
            st.recommendations.append(
                "PipeWire/PulseAudio is running. pifx opens the HAT directly (hw:N). If you get "
                "'Device or resource busy', either stop the desktop audio server for this card or run "
                "pifx with --device pipewire and pick the HAT as the default sink (wpctl set-default).")
    if st.detected and st.audio_overlay is None and not st.eeprom:
        st.recommendations.append(
            "The card is up but no DAC overlay is in config.txt, so it is probably loaded from the HAT "
            "EEPROM. That is fine.")
    return st


def format_report(st: HatStatus) -> str:
    lines = []
    lines.append(f"Board        : {st.pi_model or 'not a Raspberry Pi'}")
    if st.eeprom:
        ee = ", ".join(f"{k}={v}" for k, v in st.eeprom.items())
        lines.append(f"HAT EEPROM   : {ee}")
    else:
        lines.append("HAT EEPROM   : none read (overlay must come from config.txt)")
    lines.append(f"config.txt   : {st.config_path or 'not found'}")
    lines.append(f"DAC overlay  : {st.audio_overlay or 'none'}")
    if st.overlays:
        lines.append(f"all overlays : {', '.join(st.overlays)}")
    lines.append(f"onboard audio: {'on' if st.onboard_audio else 'off' if st.onboard_audio is not None else 'unset'}")
    lines.append(f"I2C codec    : {st.i2c_codec or 'not found'}")
    lines.append(f"modules      : {', '.join(st.modules) or 'none'}")
    servers = " ".join(n for n, on in (("pipewire", st.pipewire), ("pulseaudio", st.pulseaudio)) if on)
    lines.append(f"audio server : {servers or 'none (plain ALSA)'}")
    lines.append("sound cards  :")
    for c in st.cards:
        tag = "  <-- PCM5122 DAC" if c.is_pcm512x else ""
        hp = f"  [{c.hw_params}]" if c.hw_params else ""
        lines.append(f"    hw:{c.index}  {c.id:<16} {c.name}{hp}{tag}")
    if not st.cards:
        lines.append("    (none)")
    lines.append(f"RESULT       : {'OK - ' if st.detected else 'NOT FOUND - '}{st.summary()}")
    for i in st.issues:
        lines.append(f"  ! {i}")
    for r in st.recommendations:
        lines.append(f"  > {r}")
    return "\n".join(lines)


# --------------------------------------------------------------------------
# ALSA mixer (amixer) control
# --------------------------------------------------------------------------
@dataclass
class MixerControl:
    name: str
    numid: int
    kind: str                 # INTEGER / BOOLEAN / ENUMERATED
    values: list              # current values (per channel)
    vmin: int = 0
    vmax: int = 0
    db_min: Optional[float] = None   # dB at vmin
    db_step: Optional[float] = None  # dB per raw step
    db_mute_at_min: bool = False
    items: list = field(default_factory=list)   # enum item names

    def raw_to_db(self, raw: int) -> Optional[float]:
        if self.db_min is None or self.db_step is None:
            return None
        if self.db_mute_at_min and raw == self.vmin:
            return float("-inf")
        return self.db_min + (raw - self.vmin) * self.db_step

    def db_to_raw(self, db: float) -> int:
        if self.db_min is None or self.db_step is None or self.db_step == 0:
            raise ValueError(f"{self.name} has no dB scale")
        raw = round(self.vmin + (db - self.db_min) / self.db_step)
        return int(max(self.vmin, min(self.vmax, raw)))

    @property
    def db_max(self) -> Optional[float]:
        return self.raw_to_db(self.vmax)

    def to_dict(self) -> dict:
        d = asdict(self)
        d["db"] = [self.raw_to_db(v) if isinstance(v, int) else None for v in self.values] \
            if self.kind == "INTEGER" else None
        d["db_max"] = self.db_max
        return d


_RE_CONTROL = re.compile(r"^numid=(\d+),iface=\w+,name='([^']*)'")
_RE_TYPE = re.compile(r"type=(\w+),access=[\w-]+,values=(\d+)(?:,min=(-?\d+),max=(-?\d+))?")
_RE_VALUES = re.compile(r"^\s*:\s*values=(.*)$")
_RE_DB = re.compile(r"dBscale-min=(-?[\d.]+)dB,step=([\d.]+)dB(?:,mute=(\d))?")
_RE_ITEM = re.compile(r"^\s*;\s*Item #(\d+) '(.*)'$")


def parse_amixer_contents(text: str) -> list:
    """Parse the output of ``amixer -c N contents``."""
    controls: list[MixerControl] = []
    cur: Optional[MixerControl] = None
    for line in text.splitlines():
        m = _RE_CONTROL.match(line.strip())
        if m:
            cur = MixerControl(name=m.group(2), numid=int(m.group(1)), kind="?", values=[])
            controls.append(cur)
            continue
        if cur is None:
            continue
        m = _RE_TYPE.search(line)
        if m and cur.kind == "?":
            cur.kind = m.group(1)
            if m.group(3) is not None:
                cur.vmin, cur.vmax = int(m.group(3)), int(m.group(4))
            continue
        m = _RE_ITEM.match(line)
        if m:
            cur.items.append(m.group(2))
            continue
        m = _RE_VALUES.match(line)
        if m:
            vals = [v.strip() for v in m.group(1).split(",") if v.strip()]
            parsed: list = []
            for v in vals:
                if cur.kind == "BOOLEAN":
                    parsed.append(v.lower() == "on")
                elif cur.kind == "INTEGER":
                    try:
                        parsed.append(int(v))
                    except ValueError:
                        parsed.append(v)
                elif cur.kind == "ENUMERATED" and v.isdigit() and int(v) < len(cur.items):
                    parsed.append(cur.items[int(v)])     # report the item name, not its index
                else:
                    parsed.append(v)
            cur.values = parsed
            continue
        m = _RE_DB.search(line)
        if m:
            cur.db_min = float(m.group(1))
            cur.db_step = float(m.group(2))
            cur.db_mute_at_min = m.group(3) == "1"
    return controls


class Mixer:
    """Thin wrapper around ``amixer`` for one sound card.

    The PCM5122 driver exposes (among others):
      * 'Digital Playback Volume'  0..255, 0.5 dB steps, 207 = 0 dB, 255 = +24 dB
      * 'Digital Playback Switch'  mute
      * 'Analogue Playback Volume' 0 = -6 dB, 1 = 0 dB
      * 'Analogue Playback Boost'  0 = 0 dB, 1 = +0.8 dB
      * 'DSP Program'              interpolation filter choice
    """
    VOLUME = "Digital Playback Volume"
    MUTE = "Digital Playback Switch"
    ANALOG = "Analogue Playback Volume"
    BOOST = "Analogue Playback Boost"
    DSP = "DSP Program"

    # Never push the hardware volume past 0 dBFS gain by default: above 207 the
    # PCM5122 applies digital gain and clips anything near full scale.
    MAX_DB = 0.0

    def __init__(self, card_index: int, amixer: str = "amixer"):
        self.card = card_index
        self.amixer = shutil.which(amixer) or amixer
        self.available = shutil.which(amixer) is not None
        self.controls: dict[str, MixerControl] = {}
        self.error: Optional[str] = None
        self.refresh()

    # -- raw helpers -------------------------------------------------------
    def _run(self, *args: str) -> str:
        cmd = [self.amixer, "-c", str(self.card), *args]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=5)
        if r.returncode != 0:
            raise RuntimeError(r.stderr.strip() or f"{' '.join(cmd)} failed")
        return r.stdout

    def refresh(self) -> None:
        if not self.available:
            self.error = "amixer not installed (apt install alsa-utils)"
            return
        try:
            self.controls = {c.name: c for c in parse_amixer_contents(self._run("contents"))}
            self.error = None
        except (OSError, RuntimeError, subprocess.SubprocessError) as e:
            self.error = str(e)
            self.controls = {}

    def get(self, name: str) -> Optional[MixerControl]:
        return self.controls.get(name)

    def set_raw(self, name: str, *values) -> None:
        ctl = self.controls.get(name)
        if ctl is None:
            raise KeyError(name)
        if ctl.kind == "BOOLEAN":
            vals = ["on" if bool(v) and v != "off" else "off" for v in values]
        else:
            vals = [str(v) for v in values]
        if len(vals) == 1 and len(ctl.values) > 1:
            vals = vals * len(ctl.values)
        self._run("cset", f"numid={ctl.numid}", ",".join(vals))
        self.refresh()

    # -- high level --------------------------------------------------------
    @property
    def ok(self) -> bool:
        return self.available and self.error is None and self.VOLUME in self.controls

    def volume_db(self) -> Optional[float]:
        ctl = self.controls.get(self.VOLUME)
        if not ctl or not ctl.values:
            return None
        return ctl.raw_to_db(max(v for v in ctl.values if isinstance(v, int)))

    def set_volume_db(self, db: float, allow_boost: bool = False) -> float:
        ctl = self.controls.get(self.VOLUME)
        if not ctl:
            raise KeyError(self.VOLUME)
        cap = ctl.db_max if allow_boost else min(self.MAX_DB, ctl.db_max or self.MAX_DB)
        db = min(db, cap)
        raw = ctl.db_to_raw(db)
        self.set_raw(self.VOLUME, raw)
        return self.volume_db() or db

    def muted(self) -> Optional[bool]:
        ctl = self.controls.get(self.MUTE)
        if not ctl or not ctl.values:
            return None
        return not all(bool(v) for v in ctl.values)

    def set_mute(self, mute: bool) -> None:
        self.set_raw(self.MUTE, not mute)

    def analog_db(self) -> Optional[float]:
        ctl = self.controls.get(self.ANALOG)
        if not ctl or not ctl.values:
            return None
        return ctl.raw_to_db(min(v for v in ctl.values if isinstance(v, int)))

    def set_analog_db(self, db: float) -> None:
        """-6 dB (raw 0) or 0 dB (raw 1) on the PCM5122 analogue stage."""
        self.set_raw(self.ANALOG, 1 if db >= -3 else 0)

    def dsp_programs(self) -> list:
        ctl = self.controls.get(self.DSP)
        return list(ctl.items) if ctl else []

    def dsp_program(self) -> Optional[str]:
        ctl = self.controls.get(self.DSP)
        return ctl.values[0] if ctl and ctl.values else None

    def set_dsp_program(self, name_or_index) -> None:
        ctl = self.controls.get(self.DSP)
        if not ctl:
            raise KeyError(self.DSP)
        if isinstance(name_or_index, str) and name_or_index in ctl.items:
            name_or_index = ctl.items.index(name_or_index)
        self.set_raw(self.DSP, int(name_or_index))

    def snapshot(self) -> dict:
        """Everything the UI needs in one dict."""
        vol = self.controls.get(self.VOLUME)
        return {
            "available": self.ok,
            "error": self.error,
            "card": self.card,
            "volume_db": self.volume_db(),
            "volume_db_min": vol.db_min if vol else None,
            "volume_db_max": self.MAX_DB,
            "muted": self.muted(),
            "analog_db": self.analog_db(),
            "dsp_program": self.dsp_program(),
            "dsp_programs": self.dsp_programs(),
            "controls": sorted(self.controls),
        }


class NullMixer(Mixer):
    """Stand-in when there is no HAT (simulation / laptop development)."""

    def __init__(self):  # noqa: D107 - no amixer calls
        self.card = -1
        self.amixer = "amixer"
        self.available = False
        self.controls = {}
        self.error = "no PCM5122 card (simulation)"
        self._db = -12.0
        self._mute = False

    def refresh(self) -> None:  # pragma: no cover - trivial
        pass

    @property
    def ok(self) -> bool:
        return False

    def volume_db(self):
        return self._db

    def set_volume_db(self, db, allow_boost=False):
        self._db = float(min(db, self.MAX_DB))
        return self._db

    def muted(self):
        return self._mute

    def set_mute(self, mute):
        self._mute = bool(mute)

    def analog_db(self):
        return 0.0

    def set_analog_db(self, db):
        pass

    def dsp_programs(self):
        return []

    def dsp_program(self):
        return None

    def set_dsp_program(self, x):
        pass

    def snapshot(self) -> dict:
        return {
            "available": False, "error": self.error, "card": -1,
            "volume_db": self._db, "volume_db_min": -103.5, "volume_db_max": self.MAX_DB,
            "muted": self._mute, "analog_db": 0.0, "dsp_program": None,
            "dsp_programs": [], "controls": [],
        }


def mixer_for(status: HatStatus) -> Mixer:
    if status.detected and status.card is not None:
        return Mixer(status.card.index)
    return NullMixer()


if __name__ == "__main__":  # quick manual check: python3 -m pifx.hat
    print(format_report(detect()))
