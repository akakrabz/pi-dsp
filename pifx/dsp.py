"""Block-based stereo effects for the pifx engine.

Every effect works on float32 blocks shaped (frames, 2), keeps its own state
between blocks and smooths parameter changes so knob moves never click.
Filters run through scipy.signal.lfilter (C speed); everything else is plain
numpy. Nothing here touches hardware, so the whole chain is unit-testable.

To add an effect: subclass Effect, declare PARAMS, implement process() (or
process_wet() for effects with a tail), and add it to EFFECT_CLASSES.
"""
from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Optional

import numpy as np
from scipy.signal import lfilter

F32 = np.float32


# --------------------------------------------------------------------------
# parameter plumbing
# --------------------------------------------------------------------------
@dataclass
class ParamSpec:
    name: str
    label: str
    min: float
    max: float
    default: float
    unit: str = ""
    scale: str = "lin"          # "lin" or "log" (UI slider mapping)
    step: float = 0.0           # 0 = continuous
    choices: Optional[list] = None   # enum: list of strings; value is the index
    smooth_ms: float = 20.0     # 0 = switch instantly (enums, integers)

    def clamp(self, v):
        if self.choices is not None:
            if isinstance(v, str):
                v = self.choices.index(v) if v in self.choices else 0
            return int(max(0, min(len(self.choices) - 1, int(v))))
        v = float(v)
        if math.isnan(v):
            v = self.default
        v = max(self.min, min(self.max, v))
        if self.step:
            v = round(v / self.step) * self.step
        return v

    def to_dict(self) -> dict:
        d = self.__dict__.copy()
        return d


class Smoother:
    """Ramps a scalar toward its target over `ms` milliseconds.

    ramp(n) returns the per-sample values for the next block; value() the
    latest end point. With ms=0 it jumps immediately.
    """

    def __init__(self, value: float, sr: int, ms: float = 20.0):
        self.sr = sr
        self.ms = ms
        self.cur = float(value)
        self.target = float(value)
        self.rate = 0.0          # change per sample while ramping

    def set(self, v: float) -> None:
        self.target = float(v)
        tau = self.ms * 1e-3 * self.sr
        self.rate = (self.target - self.cur) / tau if tau > 0 else 0.0

    def snap(self, v: float) -> None:
        self.cur = self.target = float(v)
        self.rate = 0.0

    @property
    def settled(self) -> bool:
        return self.cur == self.target

    def ramp(self, n: int) -> np.ndarray:
        """Linear ramp: a change takes `ms` milliseconds whatever the block size."""
        if self.settled or self.ms <= 0:
            self.cur = self.target
            return np.full(n, self.cur, dtype=F32)
        end = self.cur + self.rate * n
        if (self.rate > 0 and end >= self.target) or (self.rate < 0 and end <= self.target):
            end = self.target
        out = np.linspace(self.cur, end, n, endpoint=False, dtype=F32)
        self.cur = end
        return out

    def step(self, n: int) -> float:
        """Advance like ramp() but only return the end value (for coefficients)."""
        return float(self.ramp(n)[-1]) if n else self.cur


# --------------------------------------------------------------------------
# biquad helpers (RBJ cookbook)
# --------------------------------------------------------------------------
def biquad(kind: str, sr: int, f0: float, q: float = 0.7071, gain_db: float = 0.0):
    f0 = max(10.0, min(f0, sr * 0.49))
    w0 = 2 * math.pi * f0 / sr
    cw, sw = math.cos(w0), math.sin(w0)
    alpha = sw / (2 * max(q, 0.05))
    A = 10 ** (gain_db / 40)
    if kind == "lowpass":
        b0, b1, b2 = (1 - cw) / 2, 1 - cw, (1 - cw) / 2
        a0, a1, a2 = 1 + alpha, -2 * cw, 1 - alpha
    elif kind == "highpass":
        b0, b1, b2 = (1 + cw) / 2, -(1 + cw), (1 + cw) / 2
        a0, a1, a2 = 1 + alpha, -2 * cw, 1 - alpha
    elif kind == "bandpass":   # constant 0 dB peak gain
        b0, b1, b2 = alpha, 0.0, -alpha
        a0, a1, a2 = 1 + alpha, -2 * cw, 1 - alpha
    elif kind == "notch":
        b0, b1, b2 = 1.0, -2 * cw, 1.0
        a0, a1, a2 = 1 + alpha, -2 * cw, 1 - alpha
    elif kind == "peak":
        b0, b1, b2 = 1 + alpha * A, -2 * cw, 1 - alpha * A
        a0, a1, a2 = 1 + alpha / A, -2 * cw, 1 - alpha / A
    elif kind == "lowshelf":
        sa = 2 * math.sqrt(A) * alpha
        b0 = A * ((A + 1) - (A - 1) * cw + sa)
        b1 = 2 * A * ((A - 1) - (A + 1) * cw)
        b2 = A * ((A + 1) - (A - 1) * cw - sa)
        a0 = (A + 1) + (A - 1) * cw + sa
        a1 = -2 * ((A - 1) + (A + 1) * cw)
        a2 = (A + 1) + (A - 1) * cw - sa
    elif kind == "highshelf":
        sa = 2 * math.sqrt(A) * alpha
        b0 = A * ((A + 1) + (A - 1) * cw + sa)
        b1 = -2 * A * ((A - 1) + (A + 1) * cw)
        b2 = A * ((A + 1) + (A - 1) * cw - sa)
        a0 = (A + 1) - (A - 1) * cw + sa
        a1 = 2 * ((A - 1) - (A + 1) * cw)
        a2 = (A + 1) - (A - 1) * cw - sa
    else:
        raise ValueError(kind)
    b = np.array([b0, b1, b2], dtype=np.float64) / a0
    a = np.array([1.0, a1 / a0, a2 / a0], dtype=np.float64)
    return b, a


class Biquad:
    """One stereo biquad section with persistent state."""

    def __init__(self, channels: int = 2):
        self.b = np.array([1.0, 0.0, 0.0])
        self.a = np.array([1.0, 0.0, 0.0])
        self.zi = np.zeros((2, channels))

    def design(self, kind, sr, f0, q=0.7071, gain_db=0.0):
        self.b, self.a = biquad(kind, sr, f0, q, gain_db)

    def __call__(self, x: np.ndarray) -> np.ndarray:
        y, self.zi = lfilter(self.b, self.a, x, axis=0, zi=self.zi)
        return y.astype(F32, copy=False)

    def reset(self):
        self.zi[:] = 0


def db_to_lin(db: float) -> float:
    return 10 ** (db / 20)


# --------------------------------------------------------------------------
# base effect
# --------------------------------------------------------------------------
class Effect:
    ID = "effect"
    NAME = "Effect"
    COLOR = "#8ab4f8"       # UI accent / Launchpad hue hint
    PARAMS: list = []
    HAS_TAIL = False        # delay/reverb: keep ringing after bypass

    def __init__(self, sr: int = 48000, blocksize: int = 256):
        self.sr = sr
        self.blocksize = blocksize
        self.enabled = False
        self._enable = Smoother(0.0, sr, 15.0)
        self._tail_quiet = True
        self.params: dict = {}
        self._smooth: dict = {}
        for p in self.PARAMS:
            self.params[p.name] = p.default
            if p.choices is None and p.smooth_ms > 0:
                self._smooth[p.name] = Smoother(p.default, sr, p.smooth_ms)
        self.setup()

    # -- to override --------------------------------------------------------
    def setup(self) -> None:
        """Allocate buffers / filters. Called once from __init__."""

    def reset(self) -> None:
        """Clear delay lines and filter memory."""

    def process(self, x: np.ndarray) -> np.ndarray:
        """Return the fully wet signal for a block (non-tail effects)."""
        return x

    def process_wet(self, x: np.ndarray) -> np.ndarray:
        """Return only the wet part to be ADDED to the dry signal (tail effects)."""
        return np.zeros_like(x)

    # -- parameter access ---------------------------------------------------
    def spec(self, name: str) -> ParamSpec:
        for p in self.PARAMS:
            if p.name == name:
                return p
        raise KeyError(name)

    def set(self, name: str, value) -> None:
        p = self.spec(name)
        v = p.clamp(value)
        self.params[name] = v
        if name in self._smooth:
            self._smooth[name].set(v)
        self.on_param(name, v)

    def on_param(self, name: str, value) -> None:
        """Hook for effects that need to react immediately to a change."""

    def set_enabled(self, on: bool) -> None:
        on = bool(on)
        if on and not self.enabled and not self.HAS_TAIL:
            self.reset()
        self.enabled = on
        self._enable.set(1.0 if on else 0.0)
        if on:
            self._tail_quiet = False

    def sm(self, name: str, n: int) -> np.ndarray:
        """Per-sample ramp for a smoothed parameter."""
        return self._smooth[name].ramp(n)

    def smv(self, name: str, n: int) -> float:
        """Advance a smoothed parameter and return its end value (for coefficients)."""
        return self._smooth[name].step(n)

    def choice(self, name: str) -> str:
        p = self.spec(name)
        return p.choices[int(self.params[name])]

    # -- the call used by the chain ------------------------------------------
    def run(self, x: np.ndarray) -> np.ndarray:
        n = len(x)
        if self.HAS_TAIL:
            if not self.enabled and self._tail_quiet and self._enable.settled:
                return x
            m = self._enable.ramp(n)[:, None]
            wet = self.process_wet(x * m)
            if not self.enabled and self._enable.settled:
                self._tail_quiet = bool(np.max(np.abs(wet)) < 1e-4)
            return x + wet
        if not self.enabled and self._enable.settled:
            return x
        m = self._enable.ramp(n)[:, None]
        wet = self.process(x)
        return x * (1 - m) + wet * m

    # -- state ----------------------------------------------------------------
    def to_dict(self) -> dict:
        return {"id": self.ID, "name": self.NAME, "color": self.COLOR,
                "enabled": self.enabled, "tail": self.HAS_TAIL,
                "params": dict(self.params),
                "schema": [p.to_dict() for p in self.PARAMS]}

    def load(self, d: dict) -> None:
        for k, v in (d.get("params") or {}).items():
            if k in self.params:
                self.set(k, v)
        if "enabled" in d:
            self.set_enabled(d["enabled"])


# --------------------------------------------------------------------------
# effects
# --------------------------------------------------------------------------
class Filter(Effect):
    ID = "filter"
    NAME = "Filter"
    COLOR = "#4fd1c5"
    PARAMS = [
        ParamSpec("mode", "Mode", 0, 2, 0, choices=["lowpass", "highpass", "bandpass"]),
        ParamSpec("cutoff", "Cutoff", 20, 20000, 1200, "Hz", scale="log", smooth_ms=30),
        ParamSpec("q", "Resonance", 0.3, 10, 0.9, "Q", scale="log", smooth_ms=30),
        ParamSpec("drive", "Drive", 0, 18, 0, "dB"),
    ]

    def setup(self):
        self.bq = Biquad()

    def reset(self):
        self.bq.reset()

    def process(self, x):
        n = len(x)
        fc = self.smv("cutoff", n)
        q = self.smv("q", n)
        self.bq.design(self.choice("mode"), self.sr, fc, q)
        g = self.sm("drive", n)[:, None]
        # kept linear on purpose (measurable transfer function); the master
        # limiter catches resonant peaks
        return self.bq(x * (10 ** (g / 20)))


class EQ(Effect):
    ID = "eq"
    NAME = "EQ"
    COLOR = "#f6ad55"
    PARAMS = [
        ParamSpec("low", "Low", -15, 15, 0, "dB"),
        ParamSpec("mid", "Mid", -15, 15, 0, "dB"),
        ParamSpec("mid_freq", "Mid freq", 200, 8000, 1000, "Hz", scale="log", smooth_ms=40),
        ParamSpec("high", "High", -15, 15, 0, "dB"),
    ]
    LOW_F = 200.0
    HIGH_F = 4000.0

    def setup(self):
        self.lo, self.mid, self.hi = Biquad(), Biquad(), Biquad()

    def reset(self):
        for b in (self.lo, self.mid, self.hi):
            b.reset()

    def process(self, x):
        n = len(x)
        self.lo.design("lowshelf", self.sr, self.LOW_F, 0.7071, self.smv("low", n))
        self.mid.design("peak", self.sr, self.smv("mid_freq", n), 1.0, self.smv("mid", n))
        self.hi.design("highshelf", self.sr, self.HIGH_F, 0.7071, self.smv("high", n))
        return self.hi(self.mid(self.lo(x)))


class Drive(Effect):
    """Overdrive / distortion. Mode picks the transfer curve.

    soft  : tanh - rounded peaks, Tube-Screamer-ish
    hard  : clip - flat tops, RAT/DS-1-ish, strong odd harmonics
    asym  : offset tanh - adds even harmonics, more 'tube'
    fold  : wavefolder - sine folds back on itself, synth-like
    """
    ID = "drive"
    NAME = "Drive"
    COLOR = "#fc8181"
    PARAMS = [
        ParamSpec("mode", "Mode", 0, 3, 0, choices=["soft", "hard", "asym", "fold"]),
        ParamSpec("drive", "Drive", 0, 40, 12, "dB"),
        ParamSpec("tone", "Tone", 500, 12000, 4000, "Hz", scale="log", smooth_ms=30),
        ParamSpec("level", "Level", -24, 6, -6, "dB"),
    ]

    def setup(self):
        self.tone = Biquad()
        self.dc = Biquad()
        self.dc.design("highpass", self.sr, 15.0, 0.7071)

    def reset(self):
        self.tone.reset()
        self.dc.reset()

    def process(self, x):
        n = len(x)
        pre = x * (10 ** (self.sm("drive", n)[:, None] / 20))
        mode = self.choice("mode")
        if mode == "soft":
            y = np.tanh(pre)
        elif mode == "hard":
            y = np.clip(pre, -0.8, 0.8) * 1.25
        elif mode == "asym":
            y = np.tanh(pre + 0.35) - math.tanh(0.35)
        else:  # fold
            y = np.sin(pre * 1.5708)
        self.tone.design("lowpass", self.sr, self.smv("tone", n), 0.7071)
        y = self.tone(self.dc(y.astype(F32)))
        return y * (10 ** (self.sm("level", n)[:, None] / 20))


class Tremolo(Effect):
    ID = "tremolo"
    NAME = "Tremolo"
    COLOR = "#b794f4"
    PARAMS = [
        ParamSpec("rate", "Rate", 0.1, 25, 4.5, "Hz", scale="log", smooth_ms=60),
        ParamSpec("depth", "Depth", 0, 1, 0.7),
        ParamSpec("shape", "Shape", 0, 2, 0, choices=["sine", "triangle", "square"]),
        ParamSpec("spread", "Stereo", 0, 180, 0, "°", smooth_ms=60),
    ]

    def setup(self):
        self.phase = 0.0

    def reset(self):
        self.phase = 0.0

    def process(self, x):
        n = len(x)
        rate = self.sm("rate", n)
        inc = rate / self.sr
        ph = self.phase + np.cumsum(inc)
        self.phase = float(ph[-1] % 1.0)
        spread = self.sm("spread", n) / 360.0
        phases = np.stack([ph % 1.0, (ph + spread) % 1.0], axis=1)
        shape = self.choice("shape")
        if shape == "sine":
            lfo = 0.5 - 0.5 * np.cos(2 * np.pi * phases)
        elif shape == "triangle":
            lfo = 1 - np.abs(2 * phases - 1)
        else:
            lfo = (phases < 0.5).astype(F32)
        depth = self.sm("depth", n)[:, None]
        gain = 1 - depth * (1 - lfo)
        return (x * gain).astype(F32)


class Delay(Effect):
    """Stereo delay with fractional (tape-style) time changes and damping."""
    ID = "delay"
    NAME = "Delay"
    COLOR = "#63b3ed"
    HAS_TAIL = True
    MAX_MS = 2000.0
    PARAMS = [
        ParamSpec("time", "Time", 10, 2000, 380, "ms", scale="log", smooth_ms=120),
        ParamSpec("feedback", "Feedback", 0, 0.98, 0.45),
        ParamSpec("mix", "Mix", 0, 1, 0.35),
        ParamSpec("damp", "Damping", 500, 16000, 5000, "Hz", scale="log", smooth_ms=60),
        ParamSpec("pingpong", "Ping-pong", 0, 1, 0, choices=["off", "on"]),
    ]

    def setup(self):
        self.L = int(self.sr * self.MAX_MS / 1000) + self.blocksize * 2 + 8
        self.buf = np.zeros((self.L, 2), dtype=F32)
        self.w = 0
        self.lp = Biquad()
        self.min_d = self.blocksize + 2

    def reset(self):
        self.buf[:] = 0
        self.lp.reset()

    def process_wet(self, x):
        n = len(x)
        d = self.sm("time", n) * (self.sr / 1000.0)
        d = np.clip(d, self.min_d, self.L - n - 4)
        pos = self.w + np.arange(n) - d
        i0 = np.floor(pos).astype(np.int64)
        frac = (pos - i0).astype(F32)[:, None]
        a = self.buf[i0 % self.L]
        b = self.buf[(i0 + 1) % self.L]
        delayed = a * (1 - frac) + b * frac
        self.lp.design("lowpass", self.sr, self.smv("damp", n), 0.7071)
        fb_sig = self.lp(delayed) * self.sm("feedback", n)[:, None]
        if self.choice("pingpong") == "on":
            fb_sig = fb_sig[:, ::-1]
        idx = (self.w + np.arange(n)) % self.L
        self.buf[idx] = x + fb_sig
        self.w = (self.w + n) % self.L
        return (delayed * self.sm("mix", n)[:, None]).astype(F32)


class Crush(Effect):
    """Bit depth and sample-rate reduction."""
    ID = "crush"
    NAME = "Bitcrush"
    COLOR = "#f687b3"
    PARAMS = [
        ParamSpec("bits", "Bits", 2, 16, 8, "bit", step=1, smooth_ms=0),
        ParamSpec("downsample", "Downsample", 1, 40, 4, "x", step=1, smooth_ms=0),
        ParamSpec("mix", "Mix", 0, 1, 1),
    ]

    def setup(self):
        self.hold = np.zeros(2, dtype=F32)
        self.count = 0

    def reset(self):
        self.hold[:] = 0
        self.count = 0

    def process(self, x):
        n = len(x)
        k = int(self.params["downsample"])
        if k > 1:
            i = np.arange(n)
            take = ((self.count + i) % k) == 0
            idx = np.where(take, i, -1)
            idx = np.maximum.accumulate(idx)
            y = np.where((idx >= 0)[:, None], x[np.maximum(idx, 0)], self.hold)
            self.hold = y[-1].copy()
            self.count = (self.count + n) % k
        else:
            y = x
        step = float(2 ** (int(self.params["bits"]) - 1))
        y = np.round(y * step) / step
        m = self.sm("mix", n)[:, None]
        return (x * (1 - m) + y * m).astype(F32)


class Stutter(Effect):
    """Beat repeat: when engaged, loops the last `size` ms of audio."""
    ID = "stutter"
    NAME = "Stutter"
    COLOR = "#f6e05e"
    PARAMS = [
        ParamSpec("size", "Slice", 15, 1000, 125, "ms", scale="log", smooth_ms=0),
        ParamSpec("decay", "Decay", 0, 1, 0, "", smooth_ms=0),
    ]

    def setup(self):
        self.hist = np.zeros((self.sr * 2, 2), dtype=F32)   # 2 s rolling history
        self.hw = 0
        self.loop: Optional[np.ndarray] = None
        self.lp = 0
        self.gain = 1.0

    def reset(self):
        pass  # keep history: it is what we want to repeat

    def _record(self, x):
        n = len(x)
        idx = (self.hw + np.arange(n)) % len(self.hist)
        self.hist[idx] = x
        self.hw = (self.hw + n) % len(self.hist)

    def set_enabled(self, on):
        if on and not self.enabled:
            size = int(self.params["size"] * self.sr / 1000)
            idx = (self.hw - size + np.arange(size)) % len(self.hist)
            self.loop = self.hist[idx].copy()
            # short fade at both ends so the loop seam does not click
            f = min(64, size // 4)
            if f > 0:
                ramp = np.linspace(0, 1, f, dtype=F32)[:, None]
                self.loop[:f] *= ramp
                self.loop[-f:] *= ramp[::-1]
            self.lp = 0
            self.gain = 1.0
        super().set_enabled(on)

    def run(self, x):
        # always record the live input so the next grab is fresh material
        self._record(x)
        return super().run(x)

    def process(self, x):
        n = len(x)
        if self.loop is None or len(self.loop) == 0:
            return x
        idx = (self.lp + np.arange(n)) % len(self.loop)
        y = self.loop[idx] * self.gain
        wrapped = (self.lp + n) // len(self.loop)
        if wrapped and self.params["decay"] > 0:
            self.gain *= (1 - 0.5 * self.params["decay"]) ** wrapped
        self.lp = (self.lp + n) % len(self.loop)
        return y.astype(F32)


class Reverb(Effect):
    """Freeverb-style: 8 damped combs in parallel into 4 allpasses, per channel."""
    ID = "reverb"
    NAME = "Reverb"
    COLOR = "#68d391"
    HAS_TAIL = True
    COMBS = (1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617)
    ALLPASS = (556, 441, 341, 225)
    SPREAD = 23
    PARAMS = [
        ParamSpec("size", "Size", 0, 1, 0.6),
        ParamSpec("damp", "Damping", 0, 1, 0.4),
        ParamSpec("mix", "Mix", 0, 1, 0.3),
        ParamSpec("predelay", "Pre-delay", 0, 120, 10, "ms", smooth_ms=0),
    ]

    def setup(self):
        scale = self.sr / 44100.0
        minlen = self.blocksize + 1
        # 16 combs: rows 0-7 left channel, 8-15 right (offset by SPREAD samples)
        self.comb_len = np.array([max(minlen, int(c * scale) + s)
                                  for s in (0, self.SPREAD) for c in self.COMBS])
        self.combs = np.zeros((16, int(self.comb_len.max())), dtype=F32)
        self.comb_w = np.zeros(16, dtype=np.int64)
        self.comb_rows = np.arange(16)[:, None]
        self.comb_lp = np.zeros((16, 1))                   # one-pole state per comb
        # 4 allpass stages, each a (2, L) buffer for both channels
        self.ap_len = [np.array([max(minlen, int(a * scale) + s) for s in (0, self.SPREAD)])
                       for a in self.ALLPASS]
        self.aps = [np.zeros((2, int(L.max())), dtype=F32) for L in self.ap_len]
        self.ap_w = [np.zeros(2, dtype=np.int64) for _ in self.ALLPASS]
        self.rows2 = np.arange(2)[:, None]
        self.pre_len = int(self.sr * 0.13) + self.blocksize
        self.pre = np.zeros((self.pre_len, 2), dtype=F32)
        self.pre_w = 0

    def reset(self):
        self.combs[:] = 0
        self.comb_lp[:] = 0
        for b in self.aps:
            b[:] = 0
        self.pre[:] = 0

    def process_wet(self, x):
        n = len(x)
        ar = np.arange(n)
        # pre-delay (integer, per block)
        pd = int(self.params["predelay"] * self.sr / 1000)
        self.pre[(self.pre_w + ar) % self.pre_len] = x
        self.pre_w = (self.pre_w + n) % self.pre_len
        xin = self.pre[(self.pre_w - pd - n + ar) % self.pre_len] if pd else x
        mono = (xin[:, 0] + xin[:, 1]) * 0.015
        fb = 0.7 + 0.28 * float(self.params["size"])
        damp = 0.1 + 0.85 * float(self.params["damp"])

        # combs, all 16 at once: read the oldest n samples of every ring
        idx = (self.comb_w[:, None] + ar[None, :]) % self.comb_len[:, None]
        y = self.combs[self.comb_rows, idx]                      # (16, n)
        lp, self.comb_lp = lfilter([1 - damp], [1.0, -damp], y, axis=1, zi=self.comb_lp)
        self.combs[self.comb_rows, idx] = mono[None, :] + lp * fb
        self.comb_w = (self.comb_w + n) % self.comb_len
        sig = np.stack([y[:8].sum(axis=0), y[8:].sum(axis=0)])  # (2, n)

        # four series allpasses, both channels at once
        for buf, w, L in zip(self.aps, self.ap_w, self.ap_len):
            idx = (w[:, None] + ar[None, :]) % L[:, None]
            bo = buf[self.rows2, idx]
            buf[self.rows2, idx] = sig + 0.5 * bo
            w[:] = (w + n) % L
            sig = bo - sig
        return (sig.T * self.sm("mix", n)[:, None]).astype(F32)


EFFECT_CLASSES = [Filter, EQ, Drive, Tremolo, Delay, Crush, Stutter, Reverb]


# --------------------------------------------------------------------------
# master section
# --------------------------------------------------------------------------
class Limiter:
    """Peak limiter + hard ceiling. Always last in the chain to protect speakers."""

    def __init__(self, sr: int, ceiling: float = 0.98, release_ms: float = 250.0):
        self.ceiling = ceiling
        self.gain = 1.0
        self.release = math.exp(-1.0 / (release_ms * 1e-3 * sr))
        self.reduction_db = 0.0

    def __call__(self, x: np.ndarray) -> np.ndarray:
        n = len(x)
        peak = float(np.max(np.abs(x))) if n else 0.0
        target = 1.0 if peak <= self.ceiling else self.ceiling / peak
        if target < self.gain:
            g_end = target                                 # instant attack
        else:
            g_end = 1.0 - (1.0 - self.gain) * (self.release ** n)
            g_end = min(g_end, target)
        g = np.linspace(self.gain, g_end, n, dtype=F32)[:, None]
        self.gain = g_end
        self.reduction_db = 20 * math.log10(max(g_end, 1e-6))
        return np.clip(x * g, -1.0, 1.0).astype(F32)


class Chain:
    """Ordered effects + software master gain + limiter."""

    def __init__(self, sr: int = 48000, blocksize: int = 256):
        self.sr = sr
        self.blocksize = blocksize
        self.effects: list[Effect] = [cls(sr, blocksize) for cls in EFFECT_CLASSES]
        self.by_id = {e.ID: e for e in self.effects}
        self.master = Smoother(db_to_lin(-6.0), sr, 20.0)
        self.master_db = -6.0
        self.bypass_all = False
        self._bypass = Smoother(1.0, sr, 15.0)
        self.limiter = Limiter(sr)

    def set_master_db(self, db: float) -> None:
        self.master_db = float(max(-60.0, min(12.0, db)))
        self.master.set(db_to_lin(self.master_db) if self.master_db > -60 else 0.0)

    def set_bypass_all(self, on: bool) -> None:
        self.bypass_all = bool(on)
        self._bypass.set(0.0 if on else 1.0)

    def process(self, x: np.ndarray) -> np.ndarray:
        n = len(x)
        y = x
        if not (self.bypass_all and self._bypass.settled):
            for fx in self.effects:
                y = fx.run(y)
            if not self._bypass.settled or self.bypass_all:
                m = self._bypass.ramp(n)[:, None]
                y = x * (1 - m) + y * m
        y = y * self.master.ramp(n)[:, None]
        return self.limiter(y)

    def to_dict(self) -> dict:
        return {"master_db": self.master_db, "bypass_all": self.bypass_all,
                "fx": [e.to_dict() for e in self.effects]}

    def load(self, d: dict) -> None:
        if "master_db" in d:
            self.set_master_db(d["master_db"])
        if "bypass_all" in d:
            self.set_bypass_all(d["bypass_all"])
        for fd in d.get("fx", []):
            fx = self.by_id.get(fd.get("id"))
            if fx:
                fx.load(fd)
