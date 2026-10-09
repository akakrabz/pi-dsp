import math
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from pifx import dsp  # noqa: E402

SR = 48000
N = 256


def sine(freq, blocks=40, amp=0.5, sr=SR, n=N):
    t = np.arange(blocks * n) / sr
    s = (amp * np.sin(2 * np.pi * freq * t)).astype(np.float32)
    return np.stack([s, s], axis=1)


def run_blocks(fx, x, n=N):
    out = [fx.run(x[i:i + n]) for i in range(0, len(x), n)]
    return np.concatenate(out)


def rms_db(x):
    return 20 * math.log10(max(float(np.sqrt(np.mean(x ** 2))), 1e-12))


def test_every_effect_runs_clean():
    for cls in dsp.EFFECT_CLASSES:
        fx = cls(SR, N)
        fx.set_enabled(True)
        x = sine(440, blocks=30)
        y = run_blocks(fx, x)
        assert y.shape == x.shape, cls.ID
        assert y.dtype == np.float32, cls.ID
        assert np.all(np.isfinite(y)), cls.ID
        # randomise every parameter and run again
        rng = np.random.default_rng(1)
        for p in cls.PARAMS:
            if p.choices is not None:
                fx.set(p.name, int(rng.integers(0, len(p.choices))))
            else:
                fx.set(p.name, float(rng.uniform(p.min, p.max)))
        y = run_blocks(fx, x)
        assert np.all(np.isfinite(y)), cls.ID


def test_bypassed_effect_is_transparent():
    for cls in dsp.EFFECT_CLASSES:
        fx = cls(SR, N)
        x = sine(1000, blocks=10)
        y = run_blocks(fx, x)
        assert np.array_equal(x, y), cls.ID


def test_lowpass_attenuates_highs():
    fx = dsp.Filter(SR, N)
    fx.set("mode", "lowpass")
    fx.set("cutoff", 500)
    fx.set_enabled(True)
    run_blocks(fx, sine(10000, blocks=40))   # let smoothing settle
    hi = run_blocks(fx, sine(10000, blocks=40))
    lo = run_blocks(fx, sine(100, blocks=40))
    assert rms_db(hi) < rms_db(sine(10000)) - 30
    assert abs(rms_db(lo) - rms_db(sine(100))) < 1.0


def test_highpass_attenuates_lows():
    fx = dsp.Filter(SR, N)
    fx.set("mode", "highpass")
    fx.set("cutoff", 2000)
    fx.set_enabled(True)
    run_blocks(fx, sine(100, blocks=40))
    lo = run_blocks(fx, sine(100, blocks=40))
    assert rms_db(lo) < rms_db(sine(100)) - 30


def test_eq_low_boost():
    fx = dsp.EQ(SR, N)
    fx.set("low", 12)
    fx.set_enabled(True)
    run_blocks(fx, sine(60, blocks=40))
    y = run_blocks(fx, sine(60, blocks=40))
    assert 10 < rms_db(y) - rms_db(sine(60)) < 13


def test_drive_adds_harmonics_and_stays_bounded():
    fx = dsp.Drive(SR, N)
    fx.set("drive", 30)
    fx.set("tone", 12000)
    fx.set("level", 0)
    fx.set_enabled(True)
    x = sine(440, blocks=60, amp=0.5)
    y = run_blocks(fx, x)[-SR:]
    # the tone filter may ring a little past the clip level; the chain's
    # limiter is what guarantees <= 1.0 at the output
    assert np.max(np.abs(y)) <= 1.5
    ch = dsp.Chain(SR, N)
    ch.by_id["drive"].load(fx.to_dict())
    ch.set_master_db(0)
    yc = run_blocks(ch, x) if False else np.concatenate([ch.process(x[i:i + N]) for i in range(0, len(x), N)])
    assert np.max(np.abs(yc)) <= 1.0
    spec = np.abs(np.fft.rfft(y[:, 0] * np.hanning(len(y))))
    freqs = np.fft.rfftfreq(len(y), 1 / SR)
    f0 = spec[np.argmin(np.abs(freqs - 440))]
    f3 = spec[np.argmin(np.abs(freqs - 1320))]
    assert f3 > f0 * 0.05, "third harmonic should be present"


def test_delay_echo_at_right_time():
    fx = dsp.Delay(SR, N)
    fx.set("time", 100)
    fx.set("feedback", 0)
    fx.set("mix", 1.0)
    fx.set("damp", 16000)
    fx.set_enabled(True)
    # settle smoothing with silence
    run_blocks(fx, np.zeros((N * 80, 2), np.float32))
    x = np.zeros((SR // 2, 2), np.float32)
    x[1000] = 1.0
    y = run_blocks(fx, x[: (len(x) // N) * N])
    peak = 1500 + int(np.argmax(np.abs(y[1500:, 0])))   # skip the dry impulse
    expected = 1000 + int(0.1 * SR)
    assert abs(peak - expected) <= 2, (peak, expected)
    # dry passes through, echo added
    assert abs(y[1000, 0] - 1.0) < 1e-5


def test_delay_tail_rings_after_bypass_then_stops():
    fx = dsp.Delay(SR, N)
    fx.set("time", 50)
    fx.set("feedback", 0.5)
    fx.set("mix", 1.0)
    fx.set_enabled(True)
    run_blocks(fx, sine(440, blocks=40))
    fx.set_enabled(False)
    tail = run_blocks(fx, np.zeros((N * 20, 2), np.float32))
    assert np.max(np.abs(tail)) > 0.01, "tail should ring"
    quiet = run_blocks(fx, np.zeros((N * 400, 2), np.float32))
    assert np.max(np.abs(quiet[-N:])) < 1e-4
    assert fx._tail_quiet


def test_crush_quantises():
    fx = dsp.Crush(SR, N)
    fx.set("bits", 3)
    fx.set("downsample", 1)
    fx.set_enabled(True)
    run_blocks(fx, sine(440, blocks=10))
    y = run_blocks(fx, sine(440, blocks=10))
    levels = np.unique(np.round(y[:, 0] * 4))
    assert len(levels) <= 9


def test_crush_downsample_holds():
    fx = dsp.Crush(SR, N)
    fx.set("bits", 16)
    fx.set("downsample", 8)
    fx.set_enabled(True)
    x = np.random.default_rng(0).standard_normal((N * 4, 2)).astype(np.float32)
    run_blocks(fx, x)
    y = run_blocks(fx, x)
    # consecutive runs of 8 equal samples (apart from the enable crossfade)
    tail = y[N * 2:]
    for i in range(0, len(tail) - 8, 8):
        seg = tail[i:i + 8, 0]
        assert np.allclose(seg, seg[0])


def test_stutter_repeats_slice():
    fx = dsp.Stutter(SR, N)
    fx.set("size", 100)
    rng = np.random.default_rng(2)
    x = rng.standard_normal((N * 100, 2)).astype(np.float32) * 0.2
    run_blocks(fx, x)                  # fill history (bypassed)
    fx.set_enabled(True)
    y = run_blocks(fx, np.zeros((N * 60, 2), np.float32))
    size = int(0.1 * SR)
    a = y[N * 4: N * 4 + size]            # after the enable crossfade
    b = y[N * 4 + size: N * 4 + 2 * size]
    assert len(a) == len(b) == size
    assert np.allclose(a, b, atol=1e-6), "loop should repeat exactly"
    assert np.max(np.abs(y)) > 0.01


def test_reverb_has_tail():
    fx = dsp.Reverb(SR, N)
    fx.set("mix", 1.0)
    fx.set("size", 0.9)
    fx.set_enabled(True)
    x = np.zeros((N * 20, 2), np.float32)
    x[100] = 0.8
    y = run_blocks(fx, x)
    late = y[N * 10:]
    assert np.max(np.abs(late)) > 1e-3
    assert np.all(np.isfinite(y))


def test_tremolo_modulates():
    fx = dsp.Tremolo(SR, N)
    fx.set("rate", 4)
    fx.set("depth", 1.0)
    fx.set_enabled(True)
    x = np.ones((SR, 2), np.float32) * 0.5
    y = run_blocks(fx, x[: (SR // N) * N])
    env = y[N * 4:, 0]
    assert env.max() > 0.49 and env.min() < 0.02


def test_enable_crossfade_has_no_jump():
    # tremolo with depth 1 starts at gain 0, so wet and dry differ a lot
    fx = dsp.Tremolo(SR, N)
    fx.set("rate", 0.1)
    fx.set("depth", 1.0)
    x = sine(200, blocks=20, amp=0.9)
    out = []
    for i in range(0, len(x), N):
        if i == N * 5:
            fx.set_enabled(True)
        if i == N * 12:
            fx.set_enabled(False)
        out.append(fx.run(x[i:i + N]))
    y = np.concatenate(out)[:, 0]
    jumps = np.abs(np.diff(y))
    # a 200 Hz 0.9 sine moves at most ~0.024 per sample; a hard switch would jump ~0.9
    assert jumps.max() < 0.03, jumps.max()
    # and the effect really did engage (signal was attenuated at some point)
    assert np.abs(y[N * 8: N * 12]).max() < 0.5


def test_limiter_caps_peaks():
    lim = dsp.Limiter(SR)
    x = np.ones((N, 2), np.float32) * 3.0
    for _ in range(5):
        y = lim(x)
    assert np.max(np.abs(y)) <= 1.0
    assert lim.reduction_db < -9


def test_chain_roundtrip_state():
    ch = dsp.Chain(SR, N)
    ch.by_id["delay"].set("mix", 0.77)
    ch.by_id["delay"].set_enabled(True)
    ch.set_master_db(-3)
    d = ch.to_dict()
    ch2 = dsp.Chain(SR, N)
    ch2.load(d)
    assert ch2.by_id["delay"].params["mix"] == 0.77
    assert ch2.by_id["delay"].enabled
    assert ch2.master_db == -3
    y = ch2.process(sine(440, blocks=1))
    assert y.shape == (N, 2)


def test_chain_speed_is_realtime():
    """All effects on at 48k/256 must process far faster than real time."""
    import time
    ch = dsp.Chain(SR, N)
    for fx in ch.effects:
        fx.set_enabled(True)
    x = sine(440, blocks=1)
    for _ in range(20):
        ch.process(x)
    t0 = time.perf_counter()
    blocks = 400
    for _ in range(blocks):
        ch.process(x)
    dt = time.perf_counter() - t0
    realtime = blocks * N / SR
    print(f"\nchain load: {100 * dt / realtime:.1f}% of real time ({dt / blocks * 1e6:.0f} us/block)")
    assert dt < realtime * 0.6


def test_smoother_settles_exactly():
    s = dsp.Smoother(0.0, SR, 20)
    s.set(1.0)
    for _ in range(200):
        s.ramp(N)
    assert s.settled and s.cur == 1.0


def test_param_clamp_and_enum():
    spec = dsp.ParamSpec("mode", "Mode", 0, 2, 0, choices=["a", "b", "c"])
    assert spec.clamp("c") == 2
    assert spec.clamp(99) == 2
    assert spec.clamp(-1) == 0
    spec2 = dsp.ParamSpec("x", "X", 0, 10, 5, step=1)
    assert spec2.clamp(3.6) == 4
    assert spec2.clamp(float("nan")) == 5
