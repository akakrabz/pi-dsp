"""Launchpad pad map: which pad does what, and what colour it shows.

Coordinates: x 0-7 left->right, y 0-7 bottom->top for the 8x8 grid.
x == 8 is the right-hand column of round buttons, y == 8 the top row.
The same map drives the physical Launchpad and the virtual one in the web UI.

The map is stored as data/padmap.json so it can be edited by hand. Actions:

  toggle      {"fx": id}                               effect on/off
  hold        {"fx": id}                               effect on while held
  hold_param  {"fx": id, "param": name, "value": v}    param while held (+ enables fx)
  param       {"fx": id, "param": name, "value": v}    set a parameter
  tone        {"wave": w, "freq": f}                   tone generator preset
  shape       {"shape": s, "a": n, "b": n, "freq": f}  X-Y scope shape
  preset      {"slot": 1-8}                            recall preset slot
  volume      {"db": v}                                hardware volume (fader step)
  volume_step {"delta": dB}                            nudge hardware volume
  mute        {}                                       toggle hardware mute
  kill        {}                                       mute while held
  bypass_all  {}                                       toggle all effects
  all_off     {}                                       every effect off
  tap         {}                                       tap tempo -> delay time
  delay_div   {"div": 0.25}                            delay = div * beat
  delay_step  {"factor": 1.25}                         multiply delay time
  source      {"kind": tone|file|capture}              select source
  file_next / file_prev {}                             step through media/
  cycle       {"fx": id, "param": name}                next enum value
  panic       {}                                       all off + defaults
"""
from __future__ import annotations

import json
from pathlib import Path

# Launchpad RGB palette indices (MK2/MK3/X/Pro). Each hue comes in
# [light, full, dim, dimmer] steps of four, so dim == full + 2.
PAL = {
    "off": 0, "grey": 1, "white": 3,
    "red": 5, "orange": 9, "yellow": 13, "lime": 17, "green": 21,
    "mint": 25, "cyan": 37, "sky": 41, "blue": 45, "violet": 49,
    "purple": 53, "pink": 57, "rose": 61,
}
FX_COLOR = {
    "filter": "cyan", "eq": "orange", "drive": "red", "tremolo": "violet",
    "delay": "blue", "crush": "pink", "stutter": "yellow", "reverb": "green",
}
FX_ORDER = ["filter", "eq", "drive", "tremolo", "delay", "crush", "stutter", "reverb"]


def dim(color: str) -> int:
    return PAL[color] + 2 if PAL[color] >= 4 else PAL[color]


def key(x: int, y: int) -> str:
    return f"{x},{y}"


def default_map() -> dict:
    m: dict = {}
    # y=7: effect toggles
    for x, fx in enumerate(FX_ORDER):
        m[key(x, 7)] = {"action": "toggle", "fx": fx, "label": fx}
    # y=6: performance holds
    holds = [
        {"action": "hold", "fx": "stutter", "label": "stutter"},
        {"action": "hold_param", "fx": "filter", "param": "cutoff", "value": 250, "label": "LP 250",
         "also": {"mode": "lowpass", "q": 1.2}},
        {"action": "hold_param", "fx": "filter", "param": "cutoff", "value": 2500, "label": "HP 2.5k",
         "also": {"mode": "highpass", "q": 1.0}},
        {"action": "hold_param", "fx": "delay", "param": "feedback", "value": 0.9, "label": "dly throw",
         "also": {"mix": 0.8}},
        {"action": "hold_param", "fx": "reverb", "param": "size", "value": 1.0, "label": "rvb freeze",
         "also": {"mix": 0.7}},
        {"action": "hold", "fx": "crush", "label": "crush"},
        {"action": "hold_param", "fx": "tremolo", "param": "rate", "value": 14, "label": "trem fast",
         "also": {"depth": 1.0, "shape": "square"}},
        {"action": "kill", "label": "kill"},
    ]
    for x, a in enumerate(holds):
        m[key(x, 6)] = a
    # y=5: tone generator presets
    tones = [("sine", 440), ("sine", 1000), ("sine", 100), ("square", 220),
             ("saw", 110), ("white", 0), ("pink", 0), ("sweep", 0)]
    for x, (w, f) in enumerate(tones):
        m[key(x, 5)] = {"action": "tone", "wave": w, "freq": f,
                        "label": f"{w} {f}" if f else w}
    # y=4: X-Y shapes for oscilloscope music
    shapes = [
        {"shape": "circle"}, {"shape": "lissajous", "a": 1, "b": 2},
        {"shape": "lissajous", "a": 2, "b": 3}, {"shape": "lissajous", "a": 3, "b": 4},
        {"shape": "rose", "a": 3}, {"shape": "rose", "a": 5},
        {"shape": "star", "a": 5}, {"shape": "square"},
    ]
    for x, s in enumerate(shapes):
        lbl = s["shape"] + (f" {s['a']}:{s['b']}" if "b" in s else f" {s['a']}" if "a" in s else "")
        m[key(x, 4)] = {"action": "shape", "freq": 100, **s, "label": lbl}
    # y=3: preset slots
    for x in range(8):
        m[key(x, 3)] = {"action": "preset", "slot": x + 1, "label": f"preset {x + 1}"}
    # y=2: delay time from tempo
    divs = [(1 / 32, "1/32"), (1 / 16, "1/16"), (1 / 12, "1/8T"), (1 / 8, "1/8"),
            (1 / 6, "1/4T"), (1 / 4, "1/4"), (3 / 8, "3/8"), (1 / 2, "1/2")]
    for x, (d, lbl) in enumerate(divs):
        m[key(x, 2)] = {"action": "delay_div", "div": d, "label": lbl}
    # y=1: filter cutoff steps
    for x, f in enumerate([80, 160, 320, 640, 1250, 2500, 5000, 10000]):
        m[key(x, 1)] = {"action": "param", "fx": "filter", "param": "cutoff", "value": f,
                        "label": f"{f} Hz" if f < 1000 else f"{f / 1000:g} kHz"}
    # y=0: hardware volume fader
    for x, db in enumerate([-40, -30, -24, -18, -12, -9, -6, 0]):
        m[key(x, 0)] = {"action": "volume", "db": db, "label": f"{db} dB"}
    # right column
    right = [
        {"action": "panic", "label": "panic"},
        {"action": "file_prev", "label": "prev file"},
        {"action": "file_next", "label": "next file"},
        {"action": "source", "kind": "capture", "label": "src: input"},
        {"action": "source", "kind": "file", "label": "src: file"},
        {"action": "source", "kind": "tone", "label": "src: tone"},
        {"action": "bypass_all", "label": "bypass"},
        {"action": "mute", "label": "mute"},
    ]
    for y, a in enumerate(right):
        m[key(8, y)] = a
    # top row
    top = [
        {"action": "volume_step", "delta": 3, "label": "vol +"},
        {"action": "volume_step", "delta": -3, "label": "vol -"},
        {"action": "delay_step", "factor": 0.8, "label": "dly -"},
        {"action": "delay_step", "factor": 1.25, "label": "dly +"},
        {"action": "tap", "label": "tap"},
        {"action": "cycle", "fx": "filter", "param": "mode", "label": "filt mode"},
        {"action": "cycle", "fx": "drive", "param": "mode", "label": "drive mode"},
        {"action": "all_off", "label": "all off"},
    ]
    for x, a in enumerate(top):
        m[key(x, 8)] = a
    return m


def load_map(path: Path) -> dict:
    path = Path(path)
    if path.exists():
        try:
            data = json.loads(path.read_text())
            if isinstance(data, dict) and data:
                return data
        except (OSError, ValueError):
            pass
    m = default_map()
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(m, indent=1))
    except OSError:
        pass
    return m


def pad_color(action: dict, ctx: dict) -> int:
    """Palette index for a pad given the current rig state (ctx from Rig.pad_context)."""
    a = action.get("action")
    fx = action.get("fx")
    col = FX_COLOR.get(fx, "white")
    held = ctx["held"].get((action.get("_x"), action.get("_y")), False)
    if a == "toggle":
        return PAL[col] if ctx["fx_on"].get(fx) else dim(col)
    if a in ("hold", "hold_param"):
        return PAL["white"] if held else dim(col)
    if a == "param":
        return PAL["cyan"] if held else PAL["grey"]
    if a == "tone":
        cur = ctx["source"]
        active = (cur.get("kind") == "tone" and cur.get("mode") == "tone"
                  and cur.get("wave") == action.get("wave")
                  and (not action.get("freq") or abs(float(cur.get("freq", 0)) - action["freq"]) < 0.5))
        return PAL["sky"] if active else dim("sky")
    if a == "shape":
        cur = ctx["source"]
        active = (cur.get("kind") == "tone" and cur.get("mode") == "shape"
                  and cur.get("shape") == action.get("shape")
                  and cur.get("a", 1) == action.get("a", cur.get("a", 1))
                  and cur.get("b", 1) == action.get("b", cur.get("b", 1)))
        return PAL["purple"] if active else dim("purple")
    if a == "preset":
        return PAL["yellow"] if ctx["preset"] == action.get("slot") else dim("yellow")
    if a == "delay_div":
        return PAL["blue"] if held else PAL["grey"]
    if a == "volume":
        vol = ctx["volume_db"]
        return PAL["green"] if vol is not None and vol >= action["db"] - 0.01 else dim("green")
    if a == "volume_step":
        return PAL["white"]
    if a == "mute":
        return PAL["red"] if ctx["muted"] else dim("red")
    if a == "kill":
        return PAL["red"] if held else dim("red")
    if a == "bypass_all":
        return PAL["orange"] if ctx["bypass_all"] else dim("orange")
    if a == "all_off":
        return dim("red")
    if a == "source":
        return PAL["lime"] if ctx["source"].get("kind") == action.get("kind") else dim("lime")
    if a in ("file_next", "file_prev"):
        return dim("lime")
    if a == "tap":
        return PAL["white"] if ctx.get("tap_flash") else PAL["grey"]
    if a in ("delay_step", "cycle"):
        return PAL["grey"]
    if a == "panic":
        return dim("red")
    return PAL["grey"]
