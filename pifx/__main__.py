"""Command line entry point.

  python3 -m pifx                 run the effects box + web UI (default: serve)
  python3 -m pifx serve [opts]    same, with options
  python3 -m pifx diag            HAT detection report and mixer state
  python3 -m pifx devices         list audio and MIDI devices
  python3 -m pifx tone [Hz] [s]   play a test tone straight to the HAT
"""
from __future__ import annotations

import argparse
import logging
import signal
import sys
import time
from pathlib import Path

from . import __version__
from . import hat as hatmod

ROOT = Path(__file__).resolve().parent.parent


def cmd_diag(args) -> int:
    st = hatmod.detect()
    print(hatmod.format_report(st))
    if st.detected and st.card:
        mx = hatmod.Mixer(st.card.index)
        print()
        if mx.ok:
            print(f"Mixer (card {st.card.index}):")
            print(f"  Digital volume : {mx.volume_db():+.1f} dB   (207 raw = 0 dB; capped at {mx.MAX_DB:+.0f} dB by pifx)")
            print(f"  Mute           : {'muted' if mx.muted() else 'on'}")
            print(f"  Analogue stage : {mx.analog_db():+.0f} dB")
            print(f"  DSP program    : {mx.dsp_program()}")
            print(f"  All controls   : {', '.join(sorted(mx.controls))}")
        else:
            print(f"Mixer: {mx.error}")
    from .engine import list_devices
    print("\nPortAudio output devices:")
    for d in list_devices():
        if "error" in d:
            print("  " + d["error"])
        elif d.get("out", 0) > 0:
            print(f"  [{d['index']:2d}] {d['name']}  ({d['out']} out)")
    return 0 if st.detected else 1


def cmd_devices(args) -> int:
    from .engine import list_devices
    print("Audio devices (PortAudio):")
    for d in list_devices():
        if "error" in d:
            print("  " + d["error"])
        else:
            print(f"  [{d['index']:2d}] in={d['in']:<2} out={d['out']:<2} {d['name']}")
    print("\nMIDI inputs:")
    try:
        import mido
        from .launchpad import identify
        for n in mido.get_input_names():
            tag = "  <-- Launchpad" if identify(n) else ""
            print(f"  {n}{tag}")
    except Exception as e:  # noqa: BLE001
        print(f"  (mido unavailable: {e})")
    return 0


def cmd_tone(args) -> int:
    import numpy as np
    import sounddevice as sd
    from .engine import resolve_device, find_hat_device
    st = hatmod.detect()
    dev = args.device
    if dev is None and st.detected and st.card:
        dev = find_hat_device(st.card.id)
    dev = resolve_device(dev)
    sr = args.rate
    t = np.arange(int(sr * args.seconds)) / sr
    amp = 10 ** (args.level / 20)
    x = (amp * np.sin(2 * np.pi * args.freq * t)).astype(np.float32)
    # fade in/out so the speakers do not pop
    f = int(sr * 0.02)
    x[:f] *= np.linspace(0, 1, f)
    x[-f:] *= np.linspace(1, 0, f)
    stereo = np.stack([x, x], axis=1)
    name = sd.query_devices(dev)["name"] if dev is not None else "default"
    print(f"playing {args.freq} Hz at {args.level} dBFS for {args.seconds}s on {name}")
    sd.play(stereo, sr, device=dev, blocking=True)
    return 0


def cmd_serve(args) -> int:
    from .rig import Rig
    from .server import serve
    from .launchpad import LaunchpadManager

    data_dir = Path(args.data) if args.data else ROOT / "data"
    media_dir = Path(args.media) if args.media else ROOT / "media"
    rig = Rig(data_dir, media_dir, sr=args.rate, blocksize=args.block)
    print(hatmod.format_report(rig.hat))
    sim = args.sim or (not rig.hat.detected and args.device is None)
    if sim and not args.sim:
        print("\nNo PCM5122 HAT found: running in simulation mode (no audio output).")
    rig.start(device=args.device, sim=sim)

    lp = None
    if not args.no_launchpad:
        lp = LaunchpadManager(rig.pad_event, rig.pad_colors)
        lp.start()
    srv = serve(rig, args.host, args.port, launchpad=lp, version=__version__)
    print(f"\npifx {__version__}: engine on {rig.engine.device_name} "
          f"({rig.engine.sr} Hz, {rig.engine.blocksize} frames, "
          f"{1000 * rig.engine.blocksize / rig.engine.sr:.1f} ms per block)")
    print(f"web UI: http://{_display_host(args.host)}:{args.port}/   (Ctrl-C to stop)")

    stop = {"flag": False}

    def _sig(*_):
        stop["flag"] = True

    signal.signal(signal.SIGINT, _sig)
    signal.signal(signal.SIGTERM, _sig)
    try:
        while not stop["flag"]:
            time.sleep(0.2)
    finally:
        print("\nstopping...")
        if lp:
            lp.stop()
        srv.shutdown()
        rig.stop()
    return 0


def _display_host(host: str) -> str:
    if host in ("0.0.0.0", ""):
        from .server import _local_ip
        return _local_ip()
    return host


def main(argv=None) -> int:
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("-v", "--verbose", action="store_true", help="debug logging")
    p = argparse.ArgumentParser(prog="pifx", description="Raspberry Pi PCM5122 HAT effects box",
                                parents=[common])
    sub = p.add_subparsers(dest="cmd")

    s = sub.add_parser("serve", help="run the engine and web UI (default)", parents=[common])
    s.add_argument("--host", default="0.0.0.0")
    s.add_argument("--port", type=int, default=8080)
    s.add_argument("--device", default=None, help="PortAudio output device index/name (default: the HAT)")
    s.add_argument("--rate", type=int, default=48000)
    s.add_argument("--block", type=int, default=256, help="frames per block (latency); try 512 if you hear dropouts")
    s.add_argument("--sim", action="store_true", help="no audio hardware: run the DSP in a paced thread")
    s.add_argument("--no-launchpad", action="store_true")
    s.add_argument("--data", default=None, help="presets + padmap directory (default: ./data)")
    s.add_argument("--media", default=None, help="wav files for the file source (default: ./media)")
    s.set_defaults(func=cmd_serve)

    d = sub.add_parser("diag", help="detect the HAT and print mixer state", parents=[common])
    d.set_defaults(func=cmd_diag)

    dv = sub.add_parser("devices", help="list audio and MIDI devices", parents=[common])
    dv.set_defaults(func=cmd_devices)

    t = sub.add_parser("tone", help="play a sine wave to the HAT", parents=[common])
    t.add_argument("freq", nargs="?", type=float, default=1000.0)
    t.add_argument("seconds", nargs="?", type=float, default=3.0)
    t.add_argument("--level", type=float, default=-12.0, help="dBFS")
    t.add_argument("--device", default=None)
    t.add_argument("--rate", type=int, default=48000)
    t.set_defaults(func=cmd_tone)

    argv = list(sys.argv[1:] if argv is None else argv)
    if not any(a in sub.choices for a in argv) and "-h" not in argv and "--help" not in argv:
        argv = ["serve"] + argv           # no sub-command given: everything is a `serve` option
    args = p.parse_args(argv)
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(name)s %(levelname)s %(message)s", datefmt="%H:%M:%S")
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
