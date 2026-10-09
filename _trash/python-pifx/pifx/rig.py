"""The Rig: single source of truth for everything the UI and the Launchpad control.

All mutations go through methods here (under one lock) and end with
`_changed()`, which notifies listeners (web clients, Launchpad LEDs). The
audio thread only *reads* effect parameters, so it never takes the lock.
"""
from __future__ import annotations

import json
import logging
import threading
import time
from pathlib import Path
from typing import Callable, Optional

from . import hat as hatmod
from . import padmap
from .dsp import Chain
from .engine import Engine, find_hat_device, hat_is_busy, pipewire_device, pipewire_set_default_sink
from .sources import ToneSource, FileSource, Source, list_media, write_test_wav, WAVES, SHAPES

log = logging.getLogger("pifx.rig")


class Rig:
    def __init__(self, data_dir: Path, media_dir: Path, sr: int = 48000, blocksize: int = 256):
        self.data_dir = Path(data_dir)
        self.media_dir = Path(media_dir)
        self.preset_dir = self.data_dir / "presets"
        self.preset_dir.mkdir(parents=True, exist_ok=True)
        self.media_dir.mkdir(parents=True, exist_ok=True)
        if not list_media(self.media_dir):
            try:
                write_test_wav(self.media_dir / "demo-tone-loop.wav", sr)
            except OSError:
                pass

        self.lock = threading.RLock()
        self.listeners: list[Callable[[str, dict], None]] = []
        self.chain = Chain(sr, blocksize)
        self.engine = Engine(self.chain, sr, blocksize)
        self.hat = hatmod.detect()
        self.mixer = hatmod.mixer_for(self.hat)
        self.map = padmap.load_map(self.data_dir / "padmap.json")
        self.held: dict = {}             # (x,y) -> restore info for hold actions
        self.tempo_bpm = 120.0
        self._taps: list[float] = []
        self.current_preset: Optional[int] = None
        self.file_index = 0
        self.tone = ToneSource(sr)
        self.engine.set_source(self.tone)
        self.started = time.time()

    # ------------------------------------------------------------------ events
    def on_change(self, fn: Callable[[str, dict], None]) -> None:
        self.listeners.append(fn)

    def _changed(self, what: str = "state", **payload) -> None:
        for fn in list(self.listeners):
            try:
                fn(what, payload)
            except Exception:  # noqa: BLE001
                log.exception("listener failed")

    # ------------------------------------------------------------------ start
    def start(self, device=None, sim: bool = False) -> None:
        self.audio_note = ""
        if device is None and not sim and self.hat.detected and self.hat.card:
            card = self.hat.card
            device = find_hat_device(card.id)
            if device is None and (self.hat.pipewire or self.hat.pulseaudio or hat_is_busy(card.id)):
                # The desktop audio server owns the card: go through it instead.
                sink = pipewire_set_default_sink(card.id) if self.hat.pipewire else None
                device = pipewire_device()
                if device is not None:
                    self.audio_note = (f"routed through PipeWire (default sink: {sink or 'unchanged'}); "
                                       f"for lowest latency run on Raspberry Pi OS Lite or stop PipeWire")
                    log.warning("HAT is held by the desktop audio server; %s", self.audio_note)
            if device is None:
                device = f"hw:CARD={card.id},DEV=0"     # direct ALSA via aplay
        try:
            self.engine.start(device=device, sim=sim)
        except Exception as e:  # noqa: BLE001
            log.error("audio device failed (%s); falling back to simulation", e)
            log.error("hint: python3 -m pifx devices   lists what PortAudio can open; try --device <name>")
            self.engine.start(sim=True)
            self.engine.device_name = f"simulation (audio failed: {e})"
        self._changed("status")

    def stop(self) -> None:
        self.engine.stop()

    # ------------------------------------------------------------------ state
    def state(self) -> dict:
        with self.lock:
            src = self.engine.source
            return {
                "chain": self.chain.to_dict(),
                "source": src.to_dict(),
                "source_kinds": ["tone", "file", "capture"],
                "waves": WAVES, "shapes": SHAPES,
                "media": list_media(self.media_dir),
                "output": {**self.mixer.snapshot(), "master_db": self.chain.master_db,
                           "limiter_db": round(self.chain.limiter.reduction_db, 1)},
                "tempo_bpm": round(self.tempo_bpm, 1),
                "preset": self.current_preset,
                "presets": self.list_presets(),
                "pads": self.pad_grid(),
            }

    def status(self) -> dict:
        return {
            "hat": self.hat.to_dict(),
            "hat_report": hatmod.format_report(self.hat),
            "engine": {**self.engine.status(), "note": getattr(self, "audio_note", "")},
            "uptime": round(time.time() - self.started),
        }

    # ------------------------------------------------------------------ effects
    def set_param(self, fx_id: str, name: str, value) -> None:
        with self.lock:
            fx = self.chain.by_id[fx_id]
            fx.set(name, value)
            self.current_preset = None
        self._changed()

    def set_enabled(self, fx_id: str, on: bool) -> None:
        with self.lock:
            self.chain.by_id[fx_id].set_enabled(on)
        self._changed()

    def set_bypass_all(self, on: bool) -> None:
        with self.lock:
            self.chain.set_bypass_all(on)
        self._changed()

    def all_off(self) -> None:
        with self.lock:
            for fx in self.chain.effects:
                fx.set_enabled(False)
        self._changed()

    def panic(self) -> None:
        with self.lock:
            for fx in self.chain.effects:
                fx.set_enabled(False)
                for p in fx.PARAMS:
                    fx.set(p.name, p.default)
            self.chain.set_bypass_all(False)
            self.held.clear()
            self.current_preset = None
        self._changed()

    def cycle(self, fx_id: str, name: str) -> None:
        with self.lock:
            fx = self.chain.by_id[fx_id]
            spec = fx.spec(name)
            if spec.choices:
                fx.set(name, (int(fx.params[name]) + 1) % len(spec.choices))
        self._changed()

    # ------------------------------------------------------------------ output
    def set_hw_volume(self, db: float) -> None:
        with self.lock:
            try:
                self.mixer.set_volume_db(float(db))
            except Exception as e:  # noqa: BLE001
                log.warning("mixer: %s", e)
        self._changed()

    def step_hw_volume(self, delta: float) -> None:
        cur = self.mixer.volume_db()
        if cur is None or cur == float("-inf"):
            cur = -40.0
        self.set_hw_volume(max(-60.0, cur + delta))

    def set_hw_mute(self, on: bool) -> None:
        with self.lock:
            try:
                self.mixer.set_mute(bool(on))
            except Exception as e:  # noqa: BLE001
                log.warning("mixer: %s", e)
        self._changed()

    def set_master_db(self, db: float) -> None:
        with self.lock:
            self.chain.set_master_db(float(db))
        self._changed()

    def set_mixer(self, control: str, value) -> None:
        """Generic hardware control: 'Analogue Playback Volume', 'DSP Program', ..."""
        with self.lock:
            if control == hatmod.Mixer.DSP:
                self.mixer.set_dsp_program(value)
            elif control == hatmod.Mixer.ANALOG:
                self.mixer.set_analog_db(float(value))
            else:
                self.mixer.set_raw(control, value)
        self._changed()

    # ------------------------------------------------------------------ source
    def set_source(self, kind: str, **params) -> None:
        with self.lock:
            if kind == "tone":
                src: Source = self.tone
                for k, v in params.items():
                    src.set(k, v)
            elif kind == "file":
                files = list_media(self.media_dir)
                if not files:
                    raise ValueError("no .wav files in media/")
                name = params.get("file") or files[min(self.file_index, len(files) - 1)]
                if name not in files:
                    raise ValueError(f"unknown file {name}")
                self.file_index = files.index(name)
                cur = self.engine.source
                if isinstance(cur, FileSource) and cur.path.name == name:
                    src = cur
                else:
                    src = FileSource(self.engine.sr, self.media_dir / name)
                for k, v in params.items():
                    if k != "file":
                        src.set(k, v)
            elif kind == "capture":
                from .sources import CaptureSource
                cur = self.engine.source
                dev = params.get("device")
                if isinstance(cur, CaptureSource) and (dev is None or cur.params["device"] == str(dev)):
                    src = cur
                else:
                    src = CaptureSource(self.engine.sr, self.engine.blocksize, device=dev)
                for k, v in params.items():
                    if k != "device":
                        src.set(k, v)
            else:
                raise ValueError(f"unknown source {kind}")
            self.engine.set_source(src)
        self._changed()

    def set_source_param(self, name: str, value) -> None:
        with self.lock:
            self.engine.source.set(name, value)
        self._changed()

    def step_file(self, delta: int) -> None:
        files = list_media(self.media_dir)
        if not files:
            return
        self.file_index = (self.file_index + delta) % len(files)
        self.set_source("file", file=files[self.file_index])

    # ------------------------------------------------------------------ tempo
    def tap(self) -> None:
        now = time.monotonic()
        with self.lock:
            self._taps = [t for t in self._taps if 0 <= now - t < 2.5] + [now]
            gaps = [b - a for a, b in zip(self._taps, self._taps[1:]) if b - a > 0.05]
            if gaps:
                self.tempo_bpm = max(30.0, min(300.0, 60.0 / (sum(gaps) / len(gaps))))
        self._changed()

    def set_tempo(self, bpm: float) -> None:
        with self.lock:
            self.tempo_bpm = max(30.0, min(300.0, float(bpm)))
        self._changed()

    def delay_div(self, div: float) -> None:
        beat_ms = 60000.0 / self.tempo_bpm
        with self.lock:
            d = self.chain.by_id["delay"]
            d.set("time", beat_ms * 4 * float(div))
            if not d.enabled:
                d.set_enabled(True)
        self._changed()

    # ------------------------------------------------------------------ presets
    def list_presets(self) -> list:
        return sorted(p.stem for p in self.preset_dir.glob("*.json"))

    def save_preset(self, name: str) -> None:
        name = _safe_name(name)
        with self.lock:
            data = {"chain": self.chain.to_dict(), "source": self.engine.source.to_dict(),
                    "tempo_bpm": self.tempo_bpm, "saved": time.time()}
            (self.preset_dir / f"{name}.json").write_text(json.dumps(data, indent=1))
            if name.startswith("slot"):
                try:
                    self.current_preset = int(name[4:])
                except ValueError:
                    pass
        self._changed()

    def load_preset(self, name: str) -> bool:
        name = _safe_name(name)
        path = self.preset_dir / f"{name}.json"
        if not path.exists():
            return False
        data = json.loads(path.read_text())
        with self.lock:
            self.chain.load(data.get("chain", {}))
            src = data.get("source") or {}
            kind = src.pop("kind", None)
            try:
                if kind == "tone":
                    self.set_source("tone", **{k: v for k, v in src.items() if k in self.tone.params})
                elif kind == "file" and src.get("file"):
                    self.set_source("file", file=src["file"], gain=src.get("gain", 0))
            except Exception as e:  # noqa: BLE001
                log.warning("preset source: %s", e)
            if "tempo_bpm" in data:
                self.tempo_bpm = float(data["tempo_bpm"])
            self.current_preset = int(name[4:]) if name.startswith("slot") and name[4:].isdigit() else None
        self._changed()
        return True

    def delete_preset(self, name: str) -> None:
        path = self.preset_dir / f"{_safe_name(name)}.json"
        if path.exists():
            path.unlink()
        self._changed()

    # ------------------------------------------------------------------ pads
    def pad_context(self) -> dict:
        return {
            "fx_on": {fx.ID: fx.enabled for fx in self.chain.effects},
            "held": {k: True for k in self.held},
            "source": self.engine.source.to_dict(),
            "preset": self.current_preset,
            "volume_db": self.mixer.volume_db(),
            "muted": bool(self.mixer.muted()),
            "bypass_all": self.chain.bypass_all,
            "tap_flash": bool(self._taps and time.monotonic() - self._taps[-1] < 0.15),
        }

    def pad_colors(self) -> dict:
        """{(x, y): palette index} for every mapped pad (unmapped pads are off)."""
        ctx = self.pad_context()
        out = {}
        for y in range(9):
            for x in range(9):
                if x == 8 and y == 8:
                    continue
                a = self.map.get(padmap.key(x, y))
                if a is None:
                    out[(x, y)] = 0
                else:
                    out[(x, y)] = padmap.pad_color({**a, "_x": x, "_y": y}, ctx)
        return out

    def pad_grid(self) -> list:
        """For the web UI: list of {x, y, action, label, color}."""
        colors = self.pad_colors()
        grid = []
        for (x, y), c in colors.items():
            a = self.map.get(padmap.key(x, y)) or {}
            grid.append({"x": x, "y": y, "action": a.get("action"), "label": a.get("label", ""),
                         "color": c, "fx": a.get("fx")})
        return grid

    def pad_event(self, x: int, y: int, pressed: bool) -> None:
        a = self.map.get(padmap.key(x, y))
        if not a:
            return
        act = a.get("action")
        try:
            if pressed:
                self._pad_press(x, y, a, act)
            else:
                self._pad_release(x, y, a, act)
        except Exception as e:  # noqa: BLE001
            log.warning("pad %s,%s %s: %s", x, y, act, e)
            self._changed()

    def _pad_press(self, x, y, a, act):
        key = (x, y)
        if act == "toggle":
            self.set_enabled(a["fx"], not self.chain.by_id[a["fx"]].enabled)
        elif act == "hold":
            fx = self.chain.by_id[a["fx"]]
            self.held[key] = {"fx": a["fx"], "was_enabled": fx.enabled}
            self.set_enabled(a["fx"], True)
        elif act == "hold_param":
            fx = self.chain.by_id[a["fx"]]
            saved = {a["param"]: fx.params[a["param"]], **{k: fx.params[k] for k in a.get("also", {})}}
            self.held[key] = {"fx": a["fx"], "was_enabled": fx.enabled, "params": saved}
            with self.lock:
                for k, v in a.get("also", {}).items():
                    fx.set(k, v)
                fx.set(a["param"], a["value"])
                fx.set_enabled(True)
            self._changed()
        elif act == "param":
            self.held[key] = {}
            self.set_param(a["fx"], a["param"], a["value"])
        elif act == "tone":
            self.set_source("tone", mode="tone", wave=a["wave"], **({"freq": a["freq"]} if a.get("freq") else {}))
        elif act == "shape":
            params = {k: a[k] for k in ("shape", "a", "b", "freq") if k in a}
            self.set_source("tone", mode="shape", **params)
        elif act == "preset":
            if not self.load_preset(f"slot{a['slot']}"):
                self._changed()
        elif act == "volume":
            self.set_hw_volume(a["db"])
        elif act == "volume_step":
            self.step_hw_volume(a["delta"])
        elif act == "mute":
            self.set_hw_mute(not self.mixer.muted())
        elif act == "kill":
            self.held[key] = {"was_muted": bool(self.mixer.muted())}
            self.set_hw_mute(True)
        elif act == "bypass_all":
            self.set_bypass_all(not self.chain.bypass_all)
        elif act == "all_off":
            self.all_off()
        elif act == "tap":
            self.tap()
        elif act == "delay_div":
            self.held[key] = {}
            self.delay_div(a["div"])
        elif act == "delay_step":
            d = self.chain.by_id["delay"]
            self.set_param("delay", "time", d.params["time"] * float(a["factor"]))
        elif act == "source":
            self.set_source(a["kind"])
        elif act == "file_next":
            self.step_file(+1)
        elif act == "file_prev":
            self.step_file(-1)
        elif act == "cycle":
            self.cycle(a["fx"], a["param"])
        elif act == "panic":
            self.panic()

    def _pad_release(self, x, y, a, act):
        info = self.held.pop((x, y), None)
        if info is None:
            return
        if act == "hold":
            self.set_enabled(info["fx"], info["was_enabled"])
        elif act == "hold_param":
            fx = self.chain.by_id[info["fx"]]
            with self.lock:
                for k, v in info["params"].items():
                    fx.set(k, v)
                fx.set_enabled(info["was_enabled"])
            self._changed()
        elif act == "kill":
            self.set_hw_mute(info["was_muted"])
        else:
            self._changed()

    # ------------------------------------------------------------------ commands
    def command(self, msg: dict) -> Optional[dict]:
        """Dispatch one JSON command (shared by WebSocket and POST /api/cmd)."""
        op = msg.get("op")
        if op == "set":
            self.set_param(msg["fx"], msg["param"], msg["value"])
        elif op == "enable":
            self.set_enabled(msg["fx"], bool(msg.get("on", True)))
        elif op == "bypass_all":
            self.set_bypass_all(bool(msg.get("on", True)))
        elif op == "all_off":
            self.all_off()
        elif op == "panic":
            self.panic()
        elif op == "hw_volume":
            self.set_hw_volume(float(msg["db"]))
        elif op == "hw_mute":
            self.set_hw_mute(bool(msg.get("on", True)))
        elif op == "master":
            self.set_master_db(float(msg["db"]))
        elif op == "mixer":
            self.set_mixer(msg["control"], msg["value"])
        elif op == "source":
            kind = msg.get("kind")
            params = {k: v for k, v in msg.items() if k not in ("op", "kind")}
            self.set_source(kind, **params)
        elif op == "source_param":
            self.set_source_param(msg["name"], msg["value"])
        elif op == "pad":
            self.pad_event(int(msg["x"]), int(msg["y"]), bool(msg.get("pressed", True)))
        elif op == "tap":
            self.tap()
        elif op == "tempo":
            self.set_tempo(float(msg["bpm"]))
        elif op == "delay_div":
            self.delay_div(float(msg["div"]))
        elif op == "preset_save":
            self.save_preset(msg.get("name") or f"slot{int(msg['slot'])}")
        elif op == "preset_load":
            self.load_preset(msg.get("name") or f"slot{int(msg['slot'])}")
        elif op == "preset_delete":
            self.delete_preset(msg["name"])
        elif op == "file_step":
            self.step_file(int(msg.get("delta", 1)))
        elif op == "rescan":
            self.hat = hatmod.detect()
            self.mixer = hatmod.mixer_for(self.hat)
            self._changed("status")
            self._changed()
        elif op == "get_state":
            return {"type": "state", "state": self.state()}
        elif op == "get_status":
            return {"type": "status", **self.status()}
        else:
            raise ValueError(f"unknown op {op!r}")
        return None


def _safe_name(name: str) -> str:
    name = "".join(c for c in str(name) if c.isalnum() or c in "-_ ").strip()
    return name[:40] or "preset"
