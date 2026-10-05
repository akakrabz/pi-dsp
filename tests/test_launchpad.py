import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from pifx import launchpad as lp  # noqa: E402
from pifx import padmap  # noqa: E402


class FakeOut:
    def __init__(self):
        self.sent = []

    def send(self, data):
        self.sent.append(list(data))

    def close(self):
        pass


class FakeIn:
    def __init__(self):
        self.callback = None

    def close(self):
        pass


def test_identify_models():
    assert lp.identify("Launchpad Mini MK3:Launchpad Mini MK3 LPMiniMK3 MIDI 20:1") == ("mk3", 0x0D)
    assert lp.identify("Launchpad X:Launchpad X LPX MIDI 24:1") == ("mk3", 0x0C)
    assert lp.identify("Launchpad Pro MK3:Launchpad Pro MK3 LPProMK3 MIDI 28:1") == ("mk3", 0x0E)
    assert lp.identify("Launchpad MK2:Launchpad MK2 MIDI 1 20:0") == ("mk2", 0x18)
    assert lp.identify("Launchpad Pro:Launchpad Pro MIDI 20:0") == ("pro1", 0x10)
    assert lp.identify("Launchpad S:Launchpad S MIDI 1 20:0") == ("legacy", None)
    assert lp.identify("Volt 1:Volt 1 MIDI 1 24:0") is None


def test_pick_port_prefers_midi_over_daw():
    names = ["Midi Through:Midi Through Port-0 14:0",
             "Launchpad X:Launchpad X LPX DAW 24:0",
             "Launchpad X:Launchpad X LPX MIDI 24:1"]
    assert lp.pick_port(names) == names[2]
    assert lp.pick_port(["Midi Through:Midi Through Port-0 14:0"]) is None
    # ALSA truncates the port names on a real Pi: "DA" / "MI"
    names = ["Launchpad Mini MK3:Launchpad Mini MK3 LPMiniMK3 DA 24:0",
             "Launchpad Mini MK3:Launchpad Mini MK3 LPMiniMK3 MI 24:1"]
    assert lp.pick_port(names) == names[1]
    assert lp.pick_port(["Launchpad MK2:Launchpad MK2 MIDI 1 20:0"]) == "Launchpad MK2:Launchpad MK2 MIDI 1 20:0"


def test_mk3_layout_roundtrip():
    lay = lp.Layout("mk3", 0x0D)
    assert lay.enter_messages() == [[0xF0, 0x00, 0x20, 0x29, 0x02, 0x0D, 0x0E, 0x01, 0xF7]]
    # grid
    assert lay.decode([0x90, 11, 100]) == (0, 0, True)       # bottom-left
    assert lay.decode([0x90, 88, 100]) == (7, 7, True)       # top-right of grid
    assert lay.decode([0x80, 45, 0]) == (4, 3, False)
    assert lay.decode([0x90, 45, 0]) == (4, 3, False)        # note-on vel 0 = release
    # right column (CC on MK3) and top row
    assert lay.decode([0xB0, 89, 127]) == (8, 7, True)
    assert lay.decode([0xB0, 19, 0]) == (8, 0, False)
    assert lay.decode([0xB0, 91, 127]) == (0, 8, True)
    assert lay.decode([0xB0, 98, 127]) == (7, 8, True)
    assert lay.decode([0xB0, 50, 1]) is None                 # not a pad
    # LEDs
    assert lay.led(0, 0, 21) == [0x90, 11, 21]
    assert lay.led(8, 7, 5) == [0xB0, 89, 5]
    assert lay.led(3, 8, 3) == [0xB0, 94, 3]


def test_mk2_layout_uses_notes_for_right_column_and_cc104():
    lay = lp.Layout("mk2", 0x18)
    assert lay.decode([0x90, 89, 100]) == (8, 7, True)
    assert lay.decode([0xB0, 104, 127]) == (0, 8, True)
    assert lay.led(8, 0, 5) == [0x90, 19, 5]
    assert lay.led(0, 8, 5) == [0xB0, 104, 5]


def test_legacy_layout():
    lay = lp.Layout("legacy", None)
    assert lay.decode([0x90, 0, 127]) == (0, 7, True)        # top-left in XY layout
    assert lay.decode([0x90, 16 * 7 + 7, 127]) == (7, 0, True)
    assert lay.decode([0x90, 8, 127]) == (8, 7, True)        # scene button
    assert lay.decode([0xB0, 104, 127]) == (0, 8, True)
    msg = lay.led(0, 7, padmap.PAL["green"])
    assert msg[0] == 0x90 and msg[1] == 0
    assert lp.legacy_color(0) == 12
    assert lp.legacy_color(padmap.PAL["red"]) == 12 + 3      # full red
    assert lp.legacy_color(padmap.PAL["green"]) == 12 + 16 * 3


def test_launchpad_press_and_leds():
    presses = []
    inp, out = FakeIn(), FakeOut()
    pad = lp.Launchpad("Launchpad Mini MK3 LPMiniMK3 MIDI", inp, out, lambda x, y, p: presses.append((x, y, p)))
    assert out.sent[0][-3:-1] == [0x0E, 0x01]                # programmer mode
    assert len(out.sent) == 1 + 80                           # then every pad cleared
    inp.callback([0x90, 81, 100])
    inp.callback([0x80, 81, 0])
    assert presses == [(0, 7, True), (0, 7, False)]
    out.sent.clear()
    pad.set_leds({(0, 7): 21, (1, 7): 23})
    assert out.sent == [[0x90, 81, 21], [0x90, 82, 23]]
    pad.set_leds({(0, 7): 21})                               # unchanged: nothing sent
    assert len(out.sent) == 2
    pad.close()
    assert out.sent[-1][-3:-1] == [0x0E, 0x00]               # back to live mode


def test_default_padmap_covers_grid_and_colors():
    m = padmap.default_map()
    assert len(m) == 80
    for y in range(9):
        for x in range(9):
            if x == 8 and y == 8:
                continue
            assert padmap.key(x, y) in m
    ctx = {"fx_on": {"filter": True, "delay": False}, "held": {}, "source": {"kind": "tone", "mode": "tone", "wave": "sine", "freq": 440.0},
           "preset": 3, "volume_db": -12.0, "muted": False, "bypass_all": False, "tap_flash": False}
    assert padmap.pad_color(m["0,7"], ctx) == padmap.PAL["cyan"]            # filter on
    assert padmap.pad_color(m["4,7"], ctx) == padmap.dim("blue")            # delay off
    assert padmap.pad_color(m["0,5"], ctx) == padmap.PAL["sky"]             # sine 440 active
    assert padmap.pad_color(m["1,5"], ctx) == padmap.dim("sky")
    assert padmap.pad_color(m["2,3"], ctx) == padmap.PAL["yellow"]          # preset 3
    assert padmap.pad_color(m["4,0"], ctx) == padmap.PAL["green"]           # -12 dB fader lit
    assert padmap.pad_color(m["5,0"], ctx) == padmap.dim("green")           # -9 dB not lit
    assert padmap.pad_color(m["8,7"], ctx) == padmap.dim("red")             # not muted


def test_padmap_file_roundtrip():
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / "padmap.json"
        m = padmap.load_map(p)
        assert p.exists() and m == padmap.default_map()
        p.write_text('{"0,0": {"action": "mute"}}')
        assert padmap.load_map(p) == {"0,0": {"action": "mute"}}
