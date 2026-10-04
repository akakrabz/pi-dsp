"""Audio sources for the engine: test tones, X-Y scope shapes, WAV files, live input.

Each source has ``pull(n) -> float32 (n, 2)`` and a small parameter dict.
The DAC HAT is output-only, so the tone/shape generator is the main way to
drive the effects (and the oscilloscope) when no USB input is connected.
"""
from __future__ import annotations

import math
import threading
import wave
from pathlib import Path

import numpy as np
from scipy.signal import lfilter, resample_poly

F32 = np.float32
TWO_PI = 2 * math.pi

WAVES = ["sine", "square", "triangle", "saw", "white", "pink", "sweep"]
SHAPES = ["circle", "lissajous", "rose", "figure8", "line", "square", "star", "spiral"]

# Paul Kellet's economy pink-noise filter (applied to white noise).
_PINK_B = [0.049922035, -0.095993537, 0.050612699, -0.004408786]
_PINK_A = [1.0, -2.494956002, 2.017265875, -0.522189400]


class Source:
    KIND = "silence"

    def __init__(self, sr: int):
        self.sr = sr
        self.params: dict = {}

    def pull(self, n: int) -> np.ndarray:
        return np.zeros((n, 2), dtype=F32)

    def set(self, name: str, value) -> None:
        if name in self.params:
            self.params[name] = value

    def to_dict(self) -> dict:
        return {"kind": self.KIND, **self.params}

    def close(self) -> None:
        pass


class ToneSource(Source):
    """Waveform generator or X-Y shape drawer.

    mode "tone":  wave/freq/level, both channels identical (sweep repeats).
    mode "shape": L and R are parametric curves so a scope in X-Y mode draws
                  the shape. `freq` is how many times per second it is traced.
    """
    KIND = "tone"

    def __init__(self, sr: int):
        super().__init__(sr)
        self.params = {
            "mode": "tone",          # tone | shape
            "wave": "sine",
            "freq": 440.0,
            "level": -12.0,          # dBFS peak
            "shape": "circle",
            "a": 3, "b": 2,          # lissajous ratio / rose petals (a)
            "phase": 90.0,           # degrees, lissajous phase offset
            "sweep_seconds": 10.0,
            "sweep_lo": 20.0, "sweep_hi": 20000.0,
        }
        self.phase = 0.0
        self.sweep_t = 0.0
        self.rng = np.random.default_rng()
        self.pink_zi = np.zeros(3)

    def set(self, name, value):
        if name in ("wave", "mode", "shape"):
            self.params[name] = str(value)
        elif name in ("a", "b"):
            self.params[name] = int(max(1, min(16, int(value))))
        elif name in self.params:
            self.params[name] = float(value)
        if name == "wave" and value == "sweep":
            self.sweep_t = 0.0

    def _amp(self) -> float:
        return 10 ** (float(self.params["level"]) / 20)

    def pull(self, n: int) -> np.ndarray:
        amp = self._amp()
        if self.params["mode"] == "shape":
            return self._shape(n, amp)
        wave_ = self.params["wave"]
        f = float(self.params["freq"])
        if wave_ == "white":
            s = self.rng.standard_normal(n).astype(F32) * (amp * 0.3)
            return np.stack([s, s], axis=1)
        if wave_ == "pink":
            w = self.rng.standard_normal(n)
            p, self.pink_zi = lfilter(_PINK_B, _PINK_A, w, zi=self.pink_zi)
            s = (p * amp * 2.0).astype(F32)
            return np.stack([s, s], axis=1)
        if wave_ == "sweep":
            T = max(0.5, float(self.params["sweep_seconds"]))
            lo, hi = float(self.params["sweep_lo"]), float(self.params["sweep_hi"])
            t = (self.sweep_t + np.arange(n) / self.sr) % T
            inst = lo * (hi / lo) ** (t / T)
            ph = self.phase + np.cumsum(inst) / self.sr
            self.phase = float(ph[-1] % 1.0)
            self.sweep_t = (self.sweep_t + n / self.sr) % T
            s = (amp * np.sin(TWO_PI * ph)).astype(F32)
            return np.stack([s, s], axis=1)
        ph = (self.phase + np.arange(n) * f / self.sr) % 1.0
        self.phase = float((self.phase + n * f / self.sr) % 1.0)
        if wave_ == "square":
            s = np.where(ph < 0.5, 1.0, -1.0)
        elif wave_ == "triangle":
            s = 1 - 4 * np.abs(ph - 0.5)
        elif wave_ == "saw":
            s = 2 * ph - 1
        else:
            s = np.sin(TWO_PI * ph)
        s = (s * amp).astype(F32)
        return np.stack([s, s], axis=1)

    def _shape(self, n: int, amp: float) -> np.ndarray:
        f = float(self.params["freq"])
        th = TWO_PI * ((self.phase + np.arange(n) * f / self.sr) % 1.0)
        self.phase = float((self.phase + n * f / self.sr) % 1.0)
        a, b = int(self.params["a"]), int(self.params["b"])
        shape = self.params["shape"]
        phi = math.radians(float(self.params["phase"]))
        if shape == "circle":
            x, y = np.cos(th), np.sin(th)
        elif shape == "lissajous":
            x, y = np.sin(a * th), np.sin(b * th + phi)
        elif shape == "rose":
            r = np.cos(a * th)
            x, y = r * np.cos(th), r * np.sin(th)
        elif shape == "figure8":
            x, y = np.sin(th), np.sin(2 * th) * 0.5
        elif shape == "line":
            x = y = np.sin(th)
        elif shape == "square":
            # trace the perimeter of a square at constant speed
            u = (th / TWO_PI) * 4.0
            seg = np.floor(u).astype(int) % 4
            t = u - np.floor(u)
            x = np.select([seg == 0, seg == 1, seg == 2, seg == 3],
                          [-1 + 2 * t, np.ones_like(t), 1 - 2 * t, -np.ones_like(t)])
            y = np.select([seg == 0, seg == 1, seg == 2, seg == 3],
                          [-np.ones_like(t), -1 + 2 * t, np.ones_like(t), 1 - 2 * t])
        elif shape == "star":
            k = max(3, a)
            r = 0.55 + 0.45 * np.cos(k * th)
            x, y = r * np.cos(th), r * np.sin(th)
        else:  # spiral: radius grows over one trace
            r = (th / TWO_PI)
            x, y = r * np.cos(a * th), r * np.sin(a * th)
        return (np.stack([x, y], axis=1) * amp).astype(F32)


class FileSource(Source):
    """Loops a WAV file (8/16/24/32-bit PCM or float), resampled to the engine rate."""
    KIND = "file"

    def __init__(self, sr: int, path: Path, loop: bool = True):
        super().__init__(sr)
        self.path = Path(path)
        self.params = {"file": self.path.name, "loop": bool(loop), "gain": 0.0, "playing": True}
        self.data = self._load(self.path, sr)
        self.pos = 0

    @staticmethod
    def _load(path: Path, sr: int) -> np.ndarray:
        with wave.open(str(path), "rb") as w:
            ch, sw, rate, nframes = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
            raw = w.readframes(nframes)
        if sw == 1:
            x = (np.frombuffer(raw, dtype=np.uint8).astype(np.float32) - 128) / 128
        elif sw == 2:
            x = np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768
        elif sw == 3:
            b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
            i = (b[:, 0].astype(np.int32) | (b[:, 1].astype(np.int32) << 8)
                 | (b[:, 2].astype(np.int32) << 16))
            i = np.where(i >= 1 << 23, i - (1 << 24), i)
            x = i.astype(np.float32) / (1 << 23)
        elif sw == 4:
            x = np.frombuffer(raw, dtype="<i4").astype(np.float32) / 2147483648
        else:
            raise ValueError(f"unsupported sample width {sw}")
        x = x.reshape(-1, ch)
        if ch == 1:
            x = np.repeat(x, 2, axis=1)
        elif ch > 2:
            x = x[:, :2]
        if rate != sr:
            g = math.gcd(rate, sr)
            x = resample_poly(x, sr // g, rate // g, axis=0).astype(np.float32)
        return np.ascontiguousarray(x, dtype=F32)

    def set(self, name, value):
        if name == "gain":
            self.params["gain"] = float(max(-40, min(12, value)))
        elif name in ("loop", "playing"):
            self.params[name] = bool(value)
        elif name == "seek":
            self.pos = int(max(0, min(len(self.data) - 1, float(value) * len(self.data))))

    @property
    def position(self) -> float:
        return self.pos / max(1, len(self.data))

    def pull(self, n: int) -> np.ndarray:
        if not self.params["playing"] or len(self.data) == 0:
            return np.zeros((n, 2), dtype=F32)
        g = 10 ** (self.params["gain"] / 20)
        if self.params["loop"]:
            idx = (self.pos + np.arange(n)) % len(self.data)
            self.pos = (self.pos + n) % len(self.data)
            return self.data[idx] * g
        out = np.zeros((n, 2), dtype=F32)
        take = min(n, len(self.data) - self.pos)
        if take > 0:
            out[:take] = self.data[self.pos:self.pos + take] * g
            self.pos += take
        else:
            self.params["playing"] = False
        return out

    def to_dict(self):
        return {**super().to_dict(), "position": round(self.position, 4),
                "seconds": round(len(self.data) / self.sr, 1)}


class CaptureSource(Source):
    """Live input from a sound device (USB interface) through a ring buffer.

    Input and output devices run on different clocks, so the buffer level
    drifts. We keep it between 1 and 6 blocks: starve -> zeros, overflow ->
    drop the oldest. Good enough for jamming; for sample-exact capture use
    the same device for in and out.
    """
    KIND = "capture"

    def __init__(self, sr: int, blocksize: int, device=None, channels: int = 2):
        super().__init__(sr)
        import sounddevice as sd  # optional dependency, only needed here
        self.params = {"device": str(device), "gain": 0.0, "channels": channels}
        self.blocksize = blocksize
        self.lock = threading.Lock()
        self.cap = max(8, 8 * blocksize)
        self.buf = np.zeros((self.cap, 2), dtype=F32)
        self.r = 0
        self.w = 0
        self.fill = 0
        self.overruns = 0
        self.underruns = 0
        self.stream = sd.InputStream(device=device, samplerate=sr, blocksize=blocksize,
                                     channels=channels, dtype="float32", callback=self._cb,
                                     latency="low")
        self.stream.start()

    def _cb(self, indata, frames, t, status):  # noqa: ARG002 - PortAudio signature
        x = indata if indata.shape[1] == 2 else np.repeat(indata[:, :1], 2, axis=1)
        with self.lock:
            if self.fill + frames > self.cap:            # overflow: drop oldest
                drop = self.fill + frames - self.cap
                self.r = (self.r + drop) % self.cap
                self.fill -= drop
                self.overruns += 1
            idx = (self.w + np.arange(frames)) % self.cap
            self.buf[idx] = x
            self.w = (self.w + frames) % self.cap
            self.fill += frames

    def set(self, name, value):
        if name == "gain":
            self.params["gain"] = float(max(-40, min(40, value)))

    def pull(self, n: int) -> np.ndarray:
        g = 10 ** (self.params["gain"] / 20)
        with self.lock:
            if self.fill < n:
                self.underruns += 1
                return np.zeros((n, 2), dtype=F32)
            # keep latency bounded: if we are far behind, skip ahead
            if self.fill > 6 * self.blocksize:
                skip = self.fill - 2 * self.blocksize
                self.r = (self.r + skip) % self.cap
                self.fill -= skip
                self.overruns += 1
            idx = (self.r + np.arange(n)) % self.cap
            out = self.buf[idx] * g
            self.r = (self.r + n) % self.cap
            self.fill -= n
        return out.astype(F32)

    def to_dict(self):
        return {**super().to_dict(), "overruns": self.overruns, "underruns": self.underruns}

    def close(self):
        try:
            self.stream.stop()
            self.stream.close()
        except Exception:  # noqa: BLE001
            pass


def list_media(media_dir: Path) -> list:
    """WAV files available to the file source."""
    try:
        return sorted(p.name for p in Path(media_dir).iterdir() if p.suffix.lower() == ".wav")
    except OSError:
        return []


def write_test_wav(path: Path, sr: int = 48000, seconds: float = 4.0) -> None:
    """Create a small demo file (two tones + rhythm) so FileSource has something to play."""
    t = np.arange(int(sr * seconds)) / sr
    beat = (np.sin(TWO_PI * 2 * t) > 0.8).astype(F32)
    melody = np.sin(TWO_PI * 220 * t) * 0.3 + np.sin(TWO_PI * 330 * t) * 0.2 * beat
    s = (melody * 0.6 * 32767).astype("<i2")
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(s.tobytes())
