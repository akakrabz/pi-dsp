"""Novation Launchpad support: detection, programmer mode, pad input, LED feedback.

Works on raw MIDI byte lists so the layout logic has no dependency on mido
(tests use fake ports). `LaunchpadManager` wraps mido/python-rtmidi, finds
the device, reconnects on hot-plug and forwards pad presses to a callback.

Supported:
  * Launchpad Mini MK3, Launchpad X, Launchpad Pro MK3   (programmer mode)
  * Launchpad MK2                                       (session layout)
  * Launchpad Pro (2015)                                (programmer layout)
  * Launchpad S / Mini MK1 / original                   (XY layout, red/green only)
"""
from __future__ import annotations

import logging
import threading
import time
from typing import Callable, Optional

log = logging.getLogger("pifx.launchpad")

SYSEX_HEAD = [0xF0, 0x00, 0x20, 0x29, 0x02]

MODELS = [
    # (substring in port name, family, sysex device id)
    ("mini mk3", "mk3", 0x0D),
    ("minimk3", "mk3", 0x0D),
    ("launchpad x", "mk3", 0x0C),
    ("lpx", "mk3", 0x0C),
    ("pro mk3", "mk3", 0x0E),
    ("lpprmk3", "mk3", 0x0E),
    ("lpprom", "mk3", 0x0E),
    ("mk2", "mk2", 0x18),
    ("launchpad pro", "pro1", 0x10),
    ("launchpad s", "legacy", None),
    ("launchpad mini", "legacy", None),
    ("launchpad", "legacy", None),
]


def identify(port_name: str) -> Optional[tuple]:
    n = port_name.lower()
    if "launchpad" not in n and "lp" not in n:
        return None
    for sub, family, dev in MODELS:
        if sub in n:
            return family, dev
    return None


def pick_port(names: list) -> Optional[str]:
    """Choose the MIDI (not DAW) port of the first Launchpad found."""
    cands = [n for n in names if identify(n)]
    if not cands:
        return None
    # MK3 devices expose "LPX DAW" and "LPX MIDI": we want the MIDI one. ALSA
    # truncates the names, so they arrive as "... LPMiniMK3 DA 24:0" / "... MI 24:1".
    def is_daw(n: str) -> bool:
        words = n.lower().replace(":", " ").split()
        return any(w in ("daw", "da", "live", "d") for w in words) or "live port" in n.lower()

    def is_midi(n: str) -> bool:
        words = n.lower().replace(":", " ").split()
        return any(w in ("midi", "mi", "m") for w in words)

    for n in cands:
        if is_midi(n) and not is_daw(n):
            return n
    for n in cands:
        if not is_daw(n):
            return n
    return cands[-1]


class Layout:
    """Translate between (x, y) pads and MIDI bytes for one Launchpad family."""

    def __init__(self, family: str, device_id: Optional[int]):
        self.family = family
        self.device_id = device_id

    # -- setup / teardown --------------------------------------------------
    def enter_messages(self) -> list:
        if self.family == "mk3":
            return [SYSEX_HEAD + [self.device_id, 0x0E, 0x01, 0xF7]]
        if self.family == "mk2":
            return [SYSEX_HEAD + [0x18, 0x22, 0x00, 0xF7]]
        if self.family == "pro1":
            return [SYSEX_HEAD + [0x10, 0x2C, 0x03, 0xF7]]
        return [[0xB0, 0x00, 0x00], [0xB0, 0x00, 0x01]]      # legacy: reset, XY layout

    def exit_messages(self) -> list:
        if self.family == "mk3":
            return [SYSEX_HEAD + [self.device_id, 0x0E, 0x00, 0xF7]]
        if self.family == "mk2":
            return [SYSEX_HEAD + [0x18, 0x0E, 0x00, 0xF7]]       # all LEDs off
        if self.family == "pro1":
            return [SYSEX_HEAD + [0x10, 0x2C, 0x00, 0xF7]]
        return [[0xB0, 0x00, 0x00]]

    # -- decode input ------------------------------------------------------
    def decode(self, msg: list) -> Optional[tuple]:
        """Return (x, y, pressed) or None."""
        if len(msg) < 3:
            return None
        status, d1, d2 = msg[0] & 0xF0, msg[1], msg[2]
        if self.family == "legacy":
            if status in (0x90, 0x80):
                row, col = d1 // 16, d1 % 16
                if row > 7 or col > 8:
                    return None
                return col, 7 - row, (status == 0x90 and d2 > 0)
            if status == 0xB0 and 104 <= d1 <= 111:
                return d1 - 104, 8, d2 > 0
            return None
        if status in (0x90, 0x80):
            tens, units = divmod(d1, 10)
            if 1 <= tens <= 8 and 1 <= units <= 9:
                return units - 1, tens - 1, (status == 0x90 and d2 > 0)
            return None
        if status == 0xB0:
            if 91 <= d1 <= 98:
                return d1 - 91, 8, d2 > 0
            if 104 <= d1 <= 111:
                return d1 - 104, 8, d2 > 0
            tens, units = divmod(d1, 10)
            if units == 9 and 1 <= tens <= 8:
                return 8, tens - 1, d2 > 0
        return None

    # -- LEDs ----------------------------------------------------------------
    def led(self, x: int, y: int, color: int) -> Optional[list]:
        """MIDI bytes to set one pad to a palette colour (static)."""
        if self.family == "legacy":
            vel = legacy_color(color)
            if y == 8:
                return [0xB0, 104 + x, vel]
            if x > 8 or y > 7:
                return None
            return [0x90, 16 * (7 - y) + x, vel]
        if y == 8:                                   # top row
            if x > 7:
                return None
            cc = (104 + x) if self.family == "mk2" else (91 + x)
            return [0xB0, cc, color]
        if x == 8:                                   # right column
            note = 10 * (y + 1) + 9
            return [0x90, note, color] if self.family == "mk2" else [0xB0, note, color]
        return [0x90, 10 * (y + 1) + (x + 1), color]


def legacy_color(pal: int) -> int:
    """Approximate an RGB palette index with the Launchpad S red/green LEDs."""
    if pal == 0:
        return 12                      # off (flags only)
    if pal in (1, 2, 3):               # greys/white -> dim amber / full amber
        r, g = (1, 1) if pal == 1 else (3, 3)
    else:
        hue = (pal - 4) // 4           # 0 red .. 14 rose
        bright = 3 if (pal - 4) % 4 in (0, 1) else 1
        if hue in (0, 14, 13):         # red / rose / pink
            r, g = bright, 0
        elif hue in (1, 2):            # orange / yellow
            r, g = bright, bright if hue == 2 else max(1, bright - 1)
        elif hue in (3, 4, 5):         # lime / green / mint
            r, g = 0, bright
        else:                          # blues & purples: amber-ish
            r, g = max(1, bright - 1), max(1, bright - 1)
    return 16 * g + r + 12


class Launchpad:
    """One connected Launchpad. Ports are duck-typed: out.send(bytes_list), in.callback."""

    def __init__(self, name: str, inport, outport, on_pad: Callable[[int, int, bool], None]):
        fam = identify(name) or ("mk3", 0x0D)
        self.name = name
        self.layout = Layout(*fam)
        self.inport = inport
        self.outport = outport
        self.on_pad = on_pad
        self.leds: dict = {}
        self._send_lock = threading.Lock()
        self.alive = True
        self.inport.callback = self._on_midi
        for m in self.layout.enter_messages():
            self.send(m)
        self.clear()

    @property
    def model(self) -> str:
        return {"mk3": "Launchpad MK3 family", "mk2": "Launchpad MK2",
                "pro1": "Launchpad Pro (2015)", "legacy": "Launchpad S / Mini"}[self.layout.family]

    def send(self, data: list) -> None:
        with self._send_lock:
            self.outport.send(data)

    def _on_midi(self, data: list) -> None:
        pad = self.layout.decode(list(data))
        if pad is not None:
            try:
                self.on_pad(*pad)
            except Exception:  # noqa: BLE001
                log.exception("pad handler failed")

    def set_led(self, x: int, y: int, color: int) -> None:
        if self.leds.get((x, y)) == color:
            return
        msg = self.layout.led(x, y, color)
        if msg:
            self.send(msg)
            self.leds[(x, y)] = color

    def set_leds(self, colors: dict) -> None:
        for (x, y), c in colors.items():
            self.set_led(x, y, c)

    def clear(self) -> None:
        for y in range(9):
            for x in range(9):
                if x == 8 and y == 8:
                    continue
                msg = self.layout.led(x, y, 0)
                if msg:
                    self.send(msg)
        self.leds = {(x, y): 0 for y in range(9) for x in range(9) if not (x == 8 and y == 8)}

    def close(self) -> None:
        self.alive = False
        try:
            self.clear()
            for m in self.layout.exit_messages():
                self.send(m)
        except Exception:  # noqa: BLE001
            pass
        for p in (self.inport, self.outport):
            try:
                p.close()
            except Exception:  # noqa: BLE001
                pass


class _MidoOut:
    def __init__(self, port):
        self.port = port

    def send(self, data):
        import mido
        self.port.send(mido.Message.from_bytes(data))

    def close(self):
        self.port.close()


class _MidoIn:
    def __init__(self, port):
        self.port = port
        self._cb = None

    @property
    def callback(self):
        return self._cb

    @callback.setter
    def callback(self, fn):
        self._cb = fn
        self.port.callback = (lambda msg: fn(msg.bytes())) if fn else None

    def close(self):
        self.port.callback = None
        self.port.close()


class LaunchpadManager:
    """Background thread: find a Launchpad, keep it connected, push LED state."""

    def __init__(self, on_pad: Callable[[int, int, bool], None],
                 colors: Callable[[], dict], poll_s: float = 2.0):
        self.on_pad = on_pad
        self.colors = colors
        self.poll_s = poll_s
        self.lp: Optional[Launchpad] = None
        self.available = False
        self.error: Optional[str] = None
        self._stop = threading.Event()
        self._dirty = threading.Event()
        self.thread = threading.Thread(target=self._run, name="pifx-launchpad", daemon=True)
        try:
            import mido  # noqa: F401
            self.available = True
        except Exception as e:  # noqa: BLE001
            self.error = f"mido not installed ({e}); Launchpad disabled"

    def start(self):
        if self.available:
            self.thread.start()
        else:
            log.warning(self.error)

    def stop(self):
        self._stop.set()
        if self.lp:
            self.lp.close()
            self.lp = None

    def refresh(self):
        """Ask the thread to re-send LED colours (call after any state change)."""
        self._dirty.set()

    def status(self) -> dict:
        return {"connected": self.lp is not None, "name": self.lp.name if self.lp else None,
                "model": self.lp.model if self.lp else None, "error": self.error,
                "available": self.available}

    def _try_connect(self) -> None:
        import mido
        names = mido.get_input_names()
        name = pick_port(names)
        if not name:
            return
        out_names = mido.get_output_names()
        out_name = name if name in out_names else next(
            (n for n in out_names if identify(n) and "daw" not in n.lower()), None)
        if not out_name:
            return
        inp = mido.open_input(name)
        outp = mido.open_output(out_name)
        self.lp = Launchpad(name, _MidoIn(inp), _MidoOut(outp), self.on_pad)
        self.error = None
        log.info("Launchpad connected: %s (%s)", name, self.lp.model)
        self._dirty.set()

    def _run(self):
        while not self._stop.is_set():
            try:
                if self.lp is None:
                    self._try_connect()
                    if self.lp is None:
                        self._stop.wait(self.poll_s)
                        continue
                if self._dirty.wait(timeout=self.poll_s):
                    self._dirty.clear()
                    self.lp.set_leds(self.colors())
                else:
                    # periodic liveness check: is the port still there?
                    import mido
                    if self.lp.name not in mido.get_input_names():
                        raise OSError("Launchpad unplugged")
            except Exception as e:  # noqa: BLE001
                self.error = str(e)
                log.warning("Launchpad: %s", e)
                if self.lp:
                    try:
                        self.lp.close()
                    except Exception:  # noqa: BLE001
                        pass
                    self.lp = None
                self._stop.wait(self.poll_s)
        time.sleep(0)
