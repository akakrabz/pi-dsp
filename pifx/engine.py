"""Real-time audio engine: source -> effects chain -> DAC HAT.

Two backends share the same per-block processing:
  * SoundDeviceBackend - PortAudio (python3-sounddevice) output to the HAT
  * SimBackend         - a paced thread with no hardware (laptop / tests)

The engine also keeps the data the UI draws: peak/RMS meters, a scope ring
buffer with the pre-FX and post-FX signals (for waveform, X-Y and transfer
curve views) and a spectrum.
"""
from __future__ import annotations

import logging
import threading
import time
from typing import Optional

import numpy as np

from .dsp import Chain
from .sources import Source

log = logging.getLogger("pifx.engine")
F32 = np.float32

SCOPE_LEN = 8192


class Engine:
    def __init__(self, chain: Chain, sr: int = 48000, blocksize: int = 256):
        self.sr = sr
        self.blocksize = blocksize
        self.chain = chain
        self.source: Source = Source(sr)
        self._lock = threading.Lock()         # guards source swaps
        self.scope = np.zeros((SCOPE_LEN, 4), dtype=F32)   # inL inR outL outR
        self.scope_w = 0
        self.peak = np.zeros(2, dtype=F32)
        self.rms = np.zeros(2, dtype=F32)
        self.in_peak = np.zeros(2, dtype=F32)
        self.load = 0.0            # fraction of the block period spent processing
        self.xruns = 0
        self.blocks = 0
        self.errors = 0
        self.backend: Optional[_Backend] = None
        self.running = False
        self.device_name = "none"

    # -- source handling ---------------------------------------------------
    def set_source(self, src: Source) -> None:
        with self._lock:
            old, self.source = self.source, src
        if old is not src:
            old.close()

    # -- the per-block hot path -------------------------------------------
    def process_block(self, n: int) -> np.ndarray:
        t0 = time.perf_counter()
        try:
            with self._lock:
                x = self.source.pull(n)
            if x.shape != (n, 2):
                x = np.zeros((n, 2), dtype=F32)
            y = self.chain.process(x)
        except Exception:  # noqa: BLE001 - never let the callback die
            self.errors += 1
            if self.errors < 5 or self.errors % 500 == 0:
                log.exception("DSP error (%d)", self.errors)
            x = np.zeros((n, 2), dtype=F32)
            y = x
        # meters & scope
        self.peak = np.max(np.abs(y), axis=0)
        self.rms = np.sqrt(np.mean(y * y, axis=0))
        self.in_peak = np.max(np.abs(x), axis=0)
        idx = (self.scope_w + np.arange(n)) % SCOPE_LEN
        self.scope[idx, 0:2] = x
        self.scope[idx, 2:4] = y
        self.scope_w = (self.scope_w + n) % SCOPE_LEN
        self.blocks += 1
        dt = time.perf_counter() - t0
        self.load = 0.9 * self.load + 0.1 * (dt / (n / self.sr))
        return y

    def scope_snapshot(self, n: int = 2048) -> np.ndarray:
        n = min(n, SCOPE_LEN)
        idx = (self.scope_w - n + np.arange(n)) % SCOPE_LEN
        return self.scope[idx].copy()

    def spectrum(self, bands: int = 64, n: int = 4096) -> np.ndarray:
        """Log-spaced band magnitudes in dBFS of the output (mono sum)."""
        snap = self.scope_snapshot(n)
        mono = (snap[:, 2] + snap[:, 3]) * 0.5
        win = np.hanning(len(mono))
        spec = np.abs(np.fft.rfft(mono * win)) * (2.0 / win.sum())
        freqs = np.fft.rfftfreq(len(mono), 1 / self.sr)
        edges = np.geomspace(20, min(20000, self.sr / 2), bands + 1)
        out = np.full(bands, -100.0, dtype=F32)
        for i in range(bands):
            sel = (freqs >= edges[i]) & (freqs < edges[i + 1])
            if sel.any():
                out[i] = 20 * np.log10(max(float(spec[sel].max()), 1e-6))
        return out

    # -- lifecycle --------------------------------------------------------------
    def start(self, device=None, sim: bool = False) -> None:
        if self.running:
            return
        if sim:
            self.backend = SimBackend(self)
        elif isinstance(device, str) and device.startswith(("alsa:", "hw:", "plughw:")):
            self.backend = AplayBackend(self, device.removeprefix("alsa:"))
        else:
            try:
                self.backend = SoundDeviceBackend(self, device)
                self.backend.start()
            except Exception as e:  # noqa: BLE001
                alsa = alsa_name_for(device)
                if not alsa:
                    raise
                log.warning("PortAudio could not open %r (%s); using aplay on %s", device, e, alsa)
                self.backend = AplayBackend(self, alsa)
            else:
                self.device_name = self.backend.name
                self.running = True
                log.info("engine running on %s @ %d Hz, %d frames", self.device_name, self.sr, self.blocksize)
                return
        self.backend.start()
        self.device_name = self.backend.name
        self.running = True
        log.info("engine running on %s @ %d Hz, %d frames", self.device_name, self.sr, self.blocksize)

    def stop(self) -> None:
        if self.backend:
            self.backend.stop()
        self.running = False
        self.source.close()

    def status(self) -> dict:
        return {
            "running": self.running, "device": self.device_name, "sr": self.sr,
            "blocksize": self.blocksize, "latency_ms": round(1000 * self.blocksize / self.sr, 2),
            "load": round(self.load, 3), "xruns": self.xruns, "errors": self.errors,
            "sim": isinstance(self.backend, SimBackend),
        }


class _Backend:
    name = "none"

    def start(self) -> None: ...
    def stop(self) -> None: ...


class SimBackend(_Backend):
    """Runs the processing loop in real time without any audio hardware."""
    name = "simulation (no audio output)"

    def __init__(self, engine: Engine):
        self.e = engine
        self._stop = threading.Event()
        self.thread = threading.Thread(target=self._run, name="pifx-sim", daemon=True)

    def start(self):
        self.thread.start()

    def stop(self):
        self._stop.set()
        self.thread.join(timeout=2)

    def _run(self):
        period = self.e.blocksize / self.e.sr
        nxt = time.perf_counter()
        while not self._stop.is_set():
            self.e.process_block(self.e.blocksize)
            nxt += period
            delay = nxt - time.perf_counter()
            if delay > 0:
                time.sleep(delay)
            else:
                nxt = time.perf_counter()   # fell behind: resync instead of bursting


class AplayBackend(_Backend):
    """Pipe raw 32-bit samples into `aplay`. No PortAudio needed; aplay's blocking write paces us.

    Uses plughw: so ALSA converts format if the card wants something other than S32_LE.
    """

    def __init__(self, engine: Engine, alsa_device: str):
        self.e = engine
        dev = alsa_device
        if dev.startswith("hw:"):
            dev = "plug" + dev
        self.dev = dev
        self.name = f"{dev} (aplay)"
        self.proc = None
        self._stop = threading.Event()
        self.thread = threading.Thread(target=self._run, name="pifx-aplay", daemon=True)

    def start(self):
        import shutil
        import subprocess
        if not shutil.which("aplay"):
            raise RuntimeError("aplay not found (sudo apt install alsa-utils)")
        n = self.e.blocksize
        self.proc = subprocess.Popen(
            ["aplay", "-q", "-D", self.dev, "-t", "raw", "-f", "S32_LE", "-c", "2",
             "-r", str(self.e.sr), f"--period-size={n}", f"--buffer-size={n * 4}"],
            stdin=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
        # A default pipe holds 64 KiB (~170 ms of audio) before writes block. Shrink it
        # to one page so knob moves are heard within ~10 ms, not ~200 ms.
        try:
            import fcntl
            fcntl.fcntl(self.proc.stdin.fileno(), 1031, 4096)   # F_SETPIPE_SZ
        except (ImportError, OSError):
            pass
        time.sleep(0.15)
        if self.proc.poll() is not None:
            err = self.proc.stderr.read().decode(errors="replace").strip()
            raise RuntimeError(f"aplay failed on {self.dev}: {err or 'exit ' + str(self.proc.returncode)}")
        self.thread.start()

    def _run(self):
        out = self.proc.stdin
        while not self._stop.is_set():
            y = self.e.process_block(self.e.blocksize)
            data = (np.clip(y, -1.0, 1.0) * 2147483647.0).astype("<i4").tobytes()
            try:
                out.write(data)
            except (BrokenPipeError, OSError):
                err = self.proc.stderr.read().decode(errors="replace").strip() if self.proc.stderr else ""
                log.error("aplay stopped: %s", err or "pipe closed")
                self.e.xruns += 1
                break

    def stop(self):
        self._stop.set()
        if self.proc:
            try:
                self.proc.stdin.close()
            except OSError:
                pass
            self.proc.terminate()
            try:
                self.proc.wait(timeout=2)
            except Exception:  # noqa: BLE001
                self.proc.kill()
        self.thread.join(timeout=2)


def alsa_name_for(device) -> Optional[str]:
    """Turn 'hw:1', 'BossDAC' or None (=the HAT) into an ALSA device string for aplay."""
    if isinstance(device, str):
        if device.startswith(("hw:", "plughw:")):
            return device
        if device and not device.isdigit():
            return f"hw:CARD={device},DEV=0"
    return None


class SoundDeviceBackend(_Backend):
    """PortAudio output stream to the HAT (or any device)."""

    def __init__(self, engine: Engine, device=None):
        self.e = engine
        self.device = device
        self.stream = None
        self.name = str(device)

    def start(self):
        import sounddevice as sd
        dev = resolve_device(self.device)
        info = sd.query_devices(dev) if dev is not None else sd.query_devices(kind="output")
        self.name = info["name"] if isinstance(info, dict) else str(info)
        self.stream = sd.OutputStream(
            device=dev, samplerate=self.e.sr, blocksize=self.e.blocksize, channels=2,
            dtype="float32", callback=self._cb, latency="low")
        self.stream.start()

    def _cb(self, outdata, frames, t, status):  # noqa: ARG002
        if status:
            self.e.xruns += 1
        outdata[:] = self.e.process_block(frames)

    def stop(self):
        if self.stream:
            self.stream.stop()
            self.stream.close()
            self.stream = None


# --------------------------------------------------------------------------
# device helpers
# --------------------------------------------------------------------------
def resolve_device(spec):
    """Accept an index, an exact name or a case-insensitive substring; None = default."""
    if spec is None or spec == "":
        return None
    import sounddevice as sd
    if isinstance(spec, int) or (isinstance(spec, str) and spec.isdigit()):
        return int(spec)
    devices = sd.query_devices()
    for i, d in enumerate(devices):
        if d["name"] == spec and d["max_output_channels"] > 0:
            return i
    for i, d in enumerate(devices):
        if spec.lower() in d["name"].lower() and d["max_output_channels"] > 0:
            return i
    raise ValueError(f"no output device matching {spec!r}")


def find_hat_device(card_id: Optional[str]) -> Optional[int]:
    """PortAudio index of the HAT's hw: device, by ALSA card id (e.g. 'BossDAC')."""
    if not card_id:
        return None
    try:
        import sounddevice as sd
        devices = sd.query_devices()
    except Exception:  # noqa: BLE001
        return None
    for i, d in enumerate(devices):
        nm = d["name"].lower()
        if card_id.lower() in nm and d["max_output_channels"] > 0 and "(hw:" in nm:
            return i
    for i, d in enumerate(devices):
        if card_id.lower() in d["name"].lower() and d["max_output_channels"] > 0:
            return i
    return None


def hat_is_busy(card_id: str) -> bool:
    """True when PortAudio sees the card but cannot open it (PipeWire/Pulse holds it)."""
    try:
        import sounddevice as sd
        return any(card_id.lower() in d["name"].lower() and d["max_output_channels"] == 0
                   for d in sd.query_devices())
    except Exception:  # noqa: BLE001
        return False


def pipewire_device():
    """PortAudio index of the 'pipewire' (or 'pulse'/'default') ALSA plugin, if present."""
    try:
        import sounddevice as sd
        devices = sd.query_devices()
    except Exception:  # noqa: BLE001
        return None
    for want in ("pipewire", "pulse", "default"):
        for i, d in enumerate(devices):
            if d["name"].lower() == want and d["max_output_channels"] > 0:
                return i
    return None


def pipewire_set_default_sink(card_hint: str) -> Optional[str]:
    """Make the HAT PipeWire's default sink with wpctl. Returns the sink name or None."""
    import re
    import shutil
    import subprocess
    if not shutil.which("wpctl"):
        return None
    try:
        out = subprocess.run(["wpctl", "status"], capture_output=True, text=True, timeout=5).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    in_sinks = False
    for line in out.splitlines():
        if "Sinks:" in line:
            in_sinks = True
            continue
        if in_sinks and ("Sources:" in line or "Filters:" in line or "Sink endpoints" in line):
            in_sinks = False
        if not in_sinks:
            continue
        m = re.search(r"(\d+)\.\s+(.*?)\s*\[", line)
        if m and card_hint.lower().replace(" ", "") in m.group(2).lower().replace(" ", ""):
            try:
                subprocess.run(["wpctl", "set-default", m.group(1)], timeout=5)
            except (OSError, subprocess.SubprocessError):
                return None
            return m.group(2)
    return None


def list_devices() -> list:
    try:
        import sounddevice as sd
    except Exception as e:  # noqa: BLE001
        return [{"error": f"sounddevice not available: {e}"}]
    out = []
    for i, d in enumerate(sd.query_devices()):
        out.append({"index": i, "name": d["name"], "in": d["max_input_channels"],
                    "out": d["max_output_channels"], "sr": d["default_samplerate"]})
    return out
