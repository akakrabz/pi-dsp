import importlib
import os
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

ASOUND_CARDS = """ 0 [vc4hdmi0       ]: vc4-hdmi - vc4-hdmi-0
                      vc4-hdmi-0
 1 [vc4hdmi1       ]: vc4-hdmi - vc4-hdmi-1
                      vc4-hdmi-1
 2 [BossDAC        ]: BossDAC - BossDAC
                      BossDAC
"""

AMIXER_CONTENTS = """numid=1,iface=MIXER,name='DSP Program'
  ; type=ENUMERATED,access=rw------,values=1,items=5
  ; Item #0 'FIR interpolation with de-emphasis'
  ; Item #1 'Low latency IIR with de-emphasis'
  ; Item #2 'High attenuation with de-emphasis'
  ; Item #3 'Fixed process flow'
  ; Item #4 'Ringing-less low latency FIR'
  : values=0
numid=4,iface=MIXER,name='Analogue Playback Boost Volume'
  ; type=INTEGER,access=rw---R--,values=2,min=0,max=1,step=0
  : values=0,0
  | dBscale-min=0.00dB,step=0.80dB,mute=0
numid=3,iface=MIXER,name='Analogue Playback Volume'
  ; type=INTEGER,access=rw---R--,values=2,min=0,max=1,step=0
  : values=1,1
  | dBscale-min=-6.00dB,step=6.00dB,mute=0
numid=5,iface=MIXER,name='Digital Playback Switch'
  ; type=BOOLEAN,access=rw------,values=2
  : values=on,on
numid=2,iface=MIXER,name='Digital Playback Volume'
  ; type=INTEGER,access=rw---R--,values=2,min=0,max=255,step=0
  : values=207,207
  | dBscale-min=-103.50dB,step=0.50dB,mute=1
"""


def make_root(tmp: Path, *, overlay=True, card=True, pi5=True, eeprom=True):
    def w(rel, text):
        p = tmp / rel.lstrip("/")
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
    if pi5:
        w("/proc/device-tree/model", "Raspberry Pi 5 Model B Rev 1.0\x00")
    if eeprom:
        w("/proc/device-tree/hat/vendor", "InnoMaker\x00")
        w("/proc/device-tree/hat/product", "HiFi DAC HAT\x00")
        w("/proc/device-tree/hat/product_id", "0x0001\x00")
    cfg = "# config\ndtparam=audio=on\n"
    if overlay:
        cfg += "dtoverlay=allo-boss-dac-pcm512x-audio  # DAC\n"
    cfg += "dtoverlay=vc4-kms-v3d\n"
    w("/boot/firmware/config.txt", cfg)
    w("/proc/modules", "snd_soc_pcm512x_i2c 12288 1 - Live 0x0\nsnd_soc_pcm512x 40960 1 snd_soc_pcm512x_i2c, Live\nsnd_soc_allo_boss_dac 16384 0 - Live\n")
    w("/sys/bus/i2c/devices/1-004d/name", "pcm5122\n")
    w("/proc/asound/cards", ASOUND_CARDS if card else ASOUND_CARDS.split(" 2 [")[0])
    w("/proc/asound/card2/pcm0p/sub0/hw_params", "access: MMAP_INTERLEAVED\nformat: S32_LE\nrate: 48000 (48000/1)\n")


def load_hat(root: Path):
    os.environ["PIFX_SYSROOT"] = str(root)
    import pifx.hat as hat
    importlib.reload(hat)
    return hat


def test_detects_boss_dac_on_pi5():
    with tempfile.TemporaryDirectory() as d:
        make_root(Path(d))
        hat = load_hat(Path(d))
        st = hat.detect()
        assert st.detected and st.card.id == "BossDAC" and st.card.index == 2
        assert st.is_pi5 and st.pi_model.startswith("Raspberry Pi 5")
        assert st.audio_overlay == "allo-boss-dac-pcm512x-audio"
        assert st.onboard_audio is True
        assert st.eeprom["vendor"] == "InnoMaker"
        assert st.i2c_codec == "1-004d: pcm5122"
        assert "snd_soc_pcm512x" in st.modules
        assert "48000" in st.card.hw_params
        assert not st.issues
        rep = hat.format_report(st)
        assert "OK - BossDAC" in rep and "<-- PCM5122" in rep


def test_recommends_overlay_when_missing():
    with tempfile.TemporaryDirectory() as d:
        make_root(Path(d), overlay=False, card=False, eeprom=False)
        hat = load_hat(Path(d))
        st = hat.detect()
        assert not st.detected
        assert any("dtoverlay=allo-boss-dac-pcm512x-audio" in r for r in st.recommendations)
        assert "/boot/firmware/config.txt" in st.recommendations[0]


def test_overlay_but_no_card_hints_pi5_slave():
    with tempfile.TemporaryDirectory() as d:
        make_root(Path(d), overlay=True, card=False)
        hat = load_hat(Path(d))
        st = hat.detect()
        assert not st.detected
        assert any("slave" in r for r in st.recommendations)
        assert any("hifiberry-dacplus" in r for r in st.recommendations)


def test_not_a_pi():
    with tempfile.TemporaryDirectory() as d:
        hat = load_hat(Path(d))
        st = hat.detect()
        assert not st.detected and "Not running on a Raspberry Pi" in st.issues[0]


def test_parse_config_variants():
    hat = load_hat(Path("/nonexistent"))
    ov, audio = hat.parse_config("dtparam=audio=off\n dtoverlay = hifiberry-dacplus,slave\n#dtoverlay=foo\n")
    assert ov == ["hifiberry-dacplus,slave"] and audio is False
    assert hat.pcm512x_overlay(ov) == "hifiberry-dacplus,slave"
    assert hat.pcm512x_overlay(["vc4-kms-v3d"]) is None


def test_parse_amixer_contents_and_db_mapping():
    hat = load_hat(Path("/nonexistent"))
    ctls = {c.name: c for c in hat.parse_amixer_contents(AMIXER_CONTENTS)}
    vol = ctls["Digital Playback Volume"]
    assert vol.kind == "INTEGER" and (vol.vmin, vol.vmax) == (0, 255)
    assert vol.values == [207, 207]
    assert vol.db_min == -103.5 and vol.db_step == 0.5 and vol.db_mute_at_min
    assert vol.raw_to_db(207) == 0.0
    assert vol.db_to_raw(0.0) == 207
    assert vol.db_to_raw(-10.0) == 187
    assert vol.db_to_raw(+99) == 255          # clamped to hardware max
    assert vol.raw_to_db(0) == float("-inf")   # mute at minimum
    sw = ctls["Digital Playback Switch"]
    assert sw.kind == "BOOLEAN" and sw.values == [True, True]
    dsp = ctls["DSP Program"]
    assert dsp.kind == "ENUMERATED" and len(dsp.items) == 5 and dsp.items[1].startswith("Low latency")
    assert dsp.values == ["FIR interpolation with de-emphasis"]
    ana = ctls["Analogue Playback Volume"]
    assert ana.raw_to_db(0) == -6.0 and ana.raw_to_db(1) == 0.0


def test_mixer_uses_amixer_and_caps_at_0db():
    """Drive Mixer through a fake amixer script that records cset calls."""
    hat = load_hat(Path("/nonexistent"))
    with tempfile.TemporaryDirectory() as d:
        script = Path(d) / "amixer"
        log = Path(d) / "log"
        script.write_text(f"""#!/bin/sh
echo "$@" >> {log}
case "$*" in
  *contents*) cat <<'EOF'
{AMIXER_CONTENTS}
EOF
  ;;
esac
""")
        script.chmod(0o755)
        mx = hat.Mixer(2, amixer=str(script))
        assert mx.ok
        assert mx.volume_db() == 0.0
        assert mx.set_volume_db(+6) == 0.0            # capped
        mx.set_volume_db(-20)
        mx.set_mute(True)
        mx.set_analog_db(-6)
        mx.set_dsp_program("Fixed process flow")
        calls = log.read_text().splitlines()
        assert any(c.endswith("cset numid=2 207,207") for c in calls)
        assert any(c.endswith("cset numid=2 167,167") for c in calls)   # -20 dB
        assert any(c.endswith("cset numid=5 off,off") for c in calls)
        assert any(c.endswith("cset numid=3 0,0") for c in calls)
        assert any(c.endswith("cset numid=1 3") for c in calls)
        snap = mx.snapshot()
        assert snap["available"] and snap["volume_db_max"] == 0.0 and len(snap["dsp_programs"]) == 5
        assert snap["dsp_program"] == "FIR interpolation with de-emphasis"


def test_null_mixer():
    hat = load_hat(Path("/nonexistent"))
    mx = hat.NullMixer()
    assert not mx.ok
    assert mx.set_volume_db(5) == 0.0
    mx.set_mute(True)
    assert mx.muted()
    assert mx.snapshot()["available"] is False


def teardown_module():
    os.environ.pop("PIFX_SYSROOT", None)
    import pifx.hat as hat
    importlib.reload(hat)
