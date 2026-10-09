# pifx — Raspberry Pi 5 + PCM5122 HAT effects box

A small Python effects processor for the InnoMaker **HiFi DAC HAT** (TI PCM5122,
dual oscillators, RCA + 3.5 mm out) on a Raspberry Pi 5, with:

- **HAT detection** — EEPROM, device-tree overlay, I2C codec, ALSA card, kernel
  modules, with plain-language fixes when something is missing
- **Output control** — the PCM5122's own digital volume (capped at 0 dB), mute,
  analogue stage, interpolation-filter choice, plus a software master and a limiter
- **Effects** — filter, 3-band EQ, drive (4 transfer curves), tremolo, delay,
  bitcrush, stutter and reverb, all click-free, in numpy/scipy
- **Sources** — test tones, sweeps, noise, X-Y shapes for oscilloscope music, WAV
  files, live input from a USB interface
- **Web UI** — one page, no build step, works on a phone; waveform / X-Y /
  transfer-curve / spectrum scope, meters, a virtual Launchpad and presets
- **Launchpad** — Mini MK3, X, Pro MK3, MK2, Pro (2015) and the old S/Mini; pads
  toggle, hold and set things, LEDs show state, hot-plug reconnects
- **Simulation mode** so everything runs on a laptop with no hardware

Only the standard library plus numpy, scipy, sounddevice, mido and python-rtmidi.
All of those come from `apt` on Raspberry Pi OS — no pip, no venv.

```
Laptop / phone / 10" panel ──HTTP+WebSocket──┐
Launchpad ──USB MIDI──┐                      │
                      ▼                      ▼
   source ─► effects chain ─► master ─► limiter ─► PCM5122 HAT ─► RCA / headphones
   (tone, X-Y shape, WAV file, USB input)
```

## 1. Hardware

- Seat the HAT on all 40 pins with the Pi **off**. Power the Pi with the official
  27 W supply if a Launchpad hangs off its USB ports.
- RCA out is **line level, 2.1 Vrms** (hotter than consumer gear). Start with the
  DAC volume low (the UI defaults to −12 dB) and raise it from there. Feed a
  guitar amp's FX return or a mixer/monitors, not an amp's guitar input.
- The 3.5 mm jack has its own headphone amplifier; use it for monitoring.
- The HAT is **output only**. For live input use a USB interface (see §5).

## 2. Install on the Pi

Fresh Raspberry Pi OS (Bookworm or newer, Lite is ideal), then:

```bash
git clone <this repo> ~/pi-wks && cd ~/pi-wks     # or copy the folder over
./install.sh            # apt packages + dtoverlay in /boot/firmware/config.txt
sudo reboot
python3 -m pifx diag    # should end with "RESULT : OK - BossDAC (card N ...)"
```

What the installer does:

1. `apt install python3-numpy python3-scipy python3-sounddevice python3-mido python3-rtmidi alsa-utils`
2. Adds `dtoverlay=allo-boss-dac-pcm512x-audio` to `config.txt`. The InnoMaker
   board is electrically the Allo Boss design (PCM5122 as I2S clock master with
   its own 45.158/49.152 MHz oscillators), so that is the matching driver.
   `hifiberry-dacplus` also works: `./install.sh --overlay hifiberry-dacplus`.
3. Adds you to the `audio` group.

`./install.sh --service` additionally installs a systemd unit so the box starts at
boot (`journalctl -u pifx -f` for logs).

If `diag` says the card is missing it prints what to try next (overlay variants,
`i2cdetect -y 1` should show the codec at 0x4d, and on a Pi 5 the `,slave`
overlay parameter if the card appears but stays silent).

## 3. Run

```bash
python3 -m pifx                      # engine + web UI on http://<pi>:8080/
python3 -m pifx --block 512          # bigger buffer if you hear dropouts
python3 -m pifx --device pipewire    # route through PipeWire on the desktop image
python3 -m pifx --sim                # no HAT: DSP runs, nothing is output
python3 -m pifx tone 1000 3          # 3 s of 1 kHz at −12 dBFS straight to the HAT
python3 -m pifx devices              # PortAudio + MIDI device list
```

Default block size is 256 frames at 48 kHz = 5.3 ms per block. With all eight
effects on, the chain uses roughly a third of one core on a Pi 5.

### The page

- **Output** — DAC volume is the PCM5122's `Digital Playback Volume` (207 raw =
  0 dB; pifx never goes above 0 dB because that is digital gain and clips).
  Master is software gain before the limiter. The limiter bar shows gain reduction.
- **Scope** — Waveform (triggered), **X-Y** (left vs right: oscilloscope music),
  **Transfer curve** (input vs output: a straight line is linear, bends are
  distortion, a loop is phase shift) and Spectrum. X-Y views auto-scale.
- **Source** — Tone (sine/square/triangle/saw/white/pink/log sweep), X-Y shape
  (circle, Lissajous a:b, rose, figure-8, square, star, spiral), File (`media/*.wav`,
  looped), Input (USB interface).
- **Effects** — each panel's round lamp is its footswitch. Double-click a slider
  to reset it. Enum parameters are buttons.
- **Launchpad** — the virtual grid mirrors the hardware; click or tap pads.
- **Presets** — 8 slots (also on the Launchpad's fourth row) plus named presets.
  Tap tempo + note divisions set the delay time.

## 4. Launchpad

Plug it into the Pi by USB. It is found automatically (and again after unplugging).
Programmer mode is used on MK3/X/Pro, the session layout on MK2.

Default layout (`data/padmap.json`, written on first run — edit it):

| Row (top→bottom) | Pads |
|---|---|
| top round buttons | vol +3 dB, vol −3 dB, delay shorter, delay longer, **tap tempo**, filter mode, drive mode, all effects off |
| 8 | toggles: filter, EQ, drive, tremolo, delay, crush, stutter, reverb |
| 7 | **hold** pads: stutter, LP 250 Hz, HP 2.5 kHz, delay throw, reverb freeze, crush, fast tremolo, kill (mute) |
| 6 | tones: sine 440 / 1 k / 100, square 220, saw 110, white, pink, sweep |
| 5 | X-Y shapes: circle, Lissajous 1:2 / 2:3 / 3:4, rose 3 / 5, star, square |
| 4 | presets 1–8 |
| 3 | delay = 1/32 … 1/2 of a bar at the tapped tempo |
| 2 | filter cutoff 80 Hz … 10 kHz |
| 1 | DAC volume fader −40 … 0 dB (lit like a meter) |
| right column (top→bottom) | mute, bypass all, source: tone / file / input, next file, prev file, panic |

Actions available in `padmap.json`: `toggle`, `hold`, `hold_param`, `param`,
`tone`, `shape`, `preset`, `volume`, `volume_step`, `mute`, `kill`, `bypass_all`,
`all_off`, `tap`, `delay_div`, `delay_step`, `source`, `file_next`, `file_prev`,
`cycle`, `panic` — see the docstring in `pifx/padmap.py`. Keys are `"x,y"` with
x 0–7 left→right, y 0–7 bottom→top; x = 8 is the right column, y = 8 the top row.

## 5. Live input (USB interface)

The HAT cannot capture. A class-compliant interface (a UA Volt, Scarlett, UCA202
…) works with no driver: pick it under Source → Input, or on the Launchpad
(`src: input`). Its clock is not the HAT's, so a small ring buffer absorbs the
drift (stats are shown as under/overruns). For sample-exact work use the same
interface for input and output: `python3 -m pifx --device "Volt"`.

## 6. Measuring with it

- **Transfer curve of an effect**: Source → sine, scope → Transfer curve, switch
  the effect on. Drive in `hard` mode shows the flat tops; `soft` the rounded S.
- **Frequency response**: Source → sweep (log, 20 Hz – 20 kHz) and record the RCA
  output into REW or Audacity; or watch the Spectrum tab.
- **Channel balance / oscilloscope music**: Source → X-Y shape → circle, scope
  (or a real scope in X-Y mode on the RCA outputs) shows an ellipse if L and R
  differ in gain or phase.
- **1 kHz check**: `python3 -m pifx tone 1000 5 --level -12` and read the RCA
  output on a multimeter: 2.1 Vrms × 10^(−12/20) ≈ 0.53 Vrms.

## 7. Control from anything (API)

Everything the page does goes through one command format, over the WebSocket at
`/ws` or `POST /api/cmd`:

```bash
curl -d '{"op":"enable","fx":"delay","on":true}'          http://pi:8080/api/cmd
curl -d '{"op":"set","fx":"filter","param":"cutoff","value":800}' http://pi:8080/api/cmd
curl -d '{"op":"hw_volume","db":-18}'                       http://pi:8080/api/cmd
curl -d '{"op":"source","kind":"tone","mode":"shape","shape":"rose","a":5,"freq":80}' http://pi:8080/api/cmd
curl -d '{"op":"pad","x":0,"y":6,"pressed":true}'           http://pi:8080/api/cmd
curl -d '{"op":"preset_load","slot":1}'                     http://pi:8080/api/cmd
curl http://pi:8080/api/state      # everything, as JSON
curl http://pi:8080/api/status     # HAT report, engine stats, Launchpad
```

Ops: `set`, `enable`, `bypass_all`, `all_off`, `panic`, `hw_volume`, `hw_mute`,
`master`, `mixer` (any ALSA control by name), `source`, `source_param`, `pad`,
`tap`, `tempo`, `delay_div`, `preset_save/load/delete`, `file_step`, `rescan`,
`get_state`, `get_status`. The server pushes `state`, `status`, `tick` and
`error` JSON messages, plus binary meter frames (~15/s) with peak/RMS, limiter
gain, DSP load, 1024 scope points × 4 channels (in L/R, out L/R) and a 64-band
spectrum. This is also the hook for an ESP32 touch panel later: it only needs a
WebSocket client and JSON.

## 8. Developing on a laptop

```bash
pip install -r requirements.txt     # or apt, as on the Pi
python3 -m pifx --sim               # http://localhost:8080/
python3 tests/run_tests.py          # 38 tests: DSP, HAT detection, mixer, Launchpad, server
python3 tests/ui_check.py           # drives the real page in headless Chromium (needs playwright)
```

`PIFX_SYSROOT=/some/fake/root python3 -m pifx diag` runs the detector against a
fake `/proc` tree — that is how the detection tests work.

### Adding an effect

```python
# pifx/dsp.py
class Chorus(Effect):
    ID, NAME, COLOR = "chorus", "Chorus", "#9ae6b4"
    PARAMS = [ParamSpec("rate", "Rate", 0.1, 5, 0.8, "Hz", scale="log"),
              ParamSpec("depth", "Depth", 0, 1, 0.5)]
    def setup(self): ...                  # buffers, filters
    def process(self, x): return wet      # (frames, 2) float32; use self.sm("rate", n) for smoothed params
EFFECT_CLASSES.append(Chorus)
```

Set `HAS_TAIL = True` and implement `process_wet()` instead for delay-like effects
whose tail should ring after bypass. Add the id to `FX_ORDER`/`FX_COLOR` in
`padmap.py` to give it a Launchpad pad. The UI builds its panel from `PARAMS`.

## 9. Troubleshooting

| Symptom | Fix |
|---|---|
| `diag`: no PCM5122 card | overlay missing → `./install.sh`, reboot. Still missing → try `--overlay hifiberry-dacplus`; check `sudo i2cdetect -y 1` for 0x4d |
| card listed but silent on a Pi 5 | append `,slave` to the overlay line |
| `Device or resource busy` | PipeWire/Pulse owns the card: `python3 -m pifx --device pipewire` (and `wpctl set-default` the HAT), or use Raspberry Pi OS Lite |
| dropouts / `xruns` climbing | `--block 512`; use the 27 W supply; `./install.sh --service` gives the engine RT priority |
| Launchpad not found | `python3 -m pifx devices` should list it under MIDI inputs; use a powered hub if its LEDs flicker |
| no sound but meters move | DAC volume at −∞ or muted (Output panel), or RCA into a guitar-level input: use an FX return |
| hum | ground loop between laptop charger, Pi and amp — run the laptop on battery or add an isolator |

## Files

```
pifx/hat.py        HAT detection + amixer mixer control
pifx/dsp.py        effects, smoothing, limiter, chain
pifx/sources.py    tone / shape / file / capture sources
pifx/engine.py     PortAudio output, meters, scope buffer, simulation backend
pifx/padmap.py     Launchpad layout + LED colour rules
pifx/launchpad.py  MIDI layer (mido), model quirks, hot-plug
pifx/rig.py        single source of truth; every command lands here
pifx/server.py     stdlib HTTP + WebSocket server
pifx/__main__.py   CLI: serve / diag / devices / tone
web/index.html     the control page
tests/             unit + end-to-end tests, headless UI check
install.sh, pifx.service
```
