"""End-to-end: Rig + server in simulation mode, driven over HTTP and a raw WebSocket."""
import base64
import json
import os
import socket
import struct
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from pifx.rig import Rig  # noqa: E402
from pifx.server import serve, ws_accept_key  # noqa: E402


class RawWS:
    """Just enough of a WebSocket client to talk to our server."""

    def __init__(self, host, port):
        self.s = socket.create_connection((host, port), timeout=5)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall((f"GET /ws HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        resp = b""
        while b"\r\n\r\n" not in resp:
            resp += self.s.recv(4096)
        head, self.buf = resp.split(b"\r\n\r\n", 1)
        assert b" 101 " in head.split(b"\r\n")[0], head
        assert ws_accept_key(key).encode() in head

    def _exact(self, n):
        while len(self.buf) < n:
            chunk = self.s.recv(65536)
            if not chunk:
                raise ConnectionError
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def recv(self):
        b1, b2 = self._exact(2)
        op, ln = b1 & 0x0F, b2 & 0x7F
        if ln == 126:
            ln = struct.unpack(">H", self._exact(2))[0]
        elif ln == 127:
            ln = struct.unpack(">Q", self._exact(8))[0]
        return op, self._exact(ln)

    def send_text(self, text):
        payload = text.encode()
        mask = os.urandom(4)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        n = len(payload)
        head = bytes([0x81]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n))
        self.s.sendall(head + mask + masked)

    def next_json(self, want_type=None, timeout=5):
        t0 = time.time()
        while time.time() - t0 < timeout:
            op, payload = self.recv()
            if op == 0x1:
                m = json.loads(payload)
                if want_type is None or m.get("type") == want_type:
                    return m
            elif op == 0x2 and want_type == "binary":
                return payload
        raise TimeoutError(want_type)

    def close(self):
        self.s.close()


def test_server_end_to_end():
    with tempfile.TemporaryDirectory() as d:
        rig = Rig(Path(d) / "data", Path(d) / "media")
        rig.start(sim=True)
        srv = serve(rig, "127.0.0.1", 0)
        port = srv.server_address[1]
        try:
            # HTTP
            with urllib.request.urlopen(f"http://127.0.0.1:{port}/") as r:
                html = r.read().decode()
                assert "<title>pifx</title>" in html
            with urllib.request.urlopen(f"http://127.0.0.1:{port}/api/state") as r:
                st = json.load(r)
                assert len(st["chain"]["fx"]) == 8 and len(st["pads"]) == 80
                assert st["media"] == ["demo-tone-loop.wav"]
            req = urllib.request.Request(f"http://127.0.0.1:{port}/api/cmd", data=b'{"op":"set","fx":"filter","param":"cutoff","value":300}',
                                         headers={"Content-Type": "application/json"})
            with urllib.request.urlopen(req) as r:
                assert json.load(r)["ok"]
            assert rig.chain.by_id["filter"].params["cutoff"] == 300

            # WebSocket
            ws = RawWS("127.0.0.1", port)
            hello = ws.next_json("hello")
            assert "version" in hello
            state = ws.next_json("state")["state"]
            assert state["chain"]["fx"][0]["params"]["cutoff"] == 300
            status = ws.next_json("status")
            assert status["engine"]["sim"] is True
            # a command that changes state -> broadcast of the new state
            ws.send_text(json.dumps({"op": "enable", "fx": "reverb", "on": True}))
            st2 = ws.next_json("state")["state"]
            assert [f for f in st2["chain"]["fx"] if f["id"] == "reverb"][0]["enabled"]
            # pad hold via websocket
            ws.send_text(json.dumps({"op": "pad", "x": 7, "y": 6, "pressed": True}))   # kill = mute while held
            st3 = ws.next_json("state")["state"]
            assert st3["output"]["muted"] is True
            ws.send_text(json.dumps({"op": "pad", "x": 7, "y": 6, "pressed": False}))
            st4 = ws.next_json("state")["state"]
            assert st4["output"]["muted"] is False
            # source change over ws
            ws.send_text(json.dumps({"op": "source", "kind": "file"}))
            st5 = ws.next_json("state")["state"]
            assert st5["source"]["kind"] == "file" and st5["source"]["file"] == "demo-tone-loop.wav"
            # binary meter frame
            frame = ws.next_json("binary")
            kind, nch, npts, nb = struct.unpack_from("<BBHH", frame)
            assert kind == 1 and nch == 4 and npts == 1024 and nb == 64
            assert len(frame) == 38 + npts * nch * 2 + nb * 2
            # bad command -> error message, connection stays up
            ws.send_text(json.dumps({"op": "nope"}))
            err = ws.next_json("error")
            assert "unknown op" in err["error"]
            # presets
            ws.send_text(json.dumps({"op": "preset_save", "slot": 2}))
            st6 = ws.next_json("state")["state"]
            assert "slot2" in st6["presets"] and st6["preset"] == 2
            ws.close()
        finally:
            srv.shutdown()
            rig.stop()


def test_rig_tap_tempo_and_delay_div():
    with tempfile.TemporaryDirectory() as d:
        rig = Rig(Path(d) / "data", Path(d) / "media")
        now = time.monotonic()
        rig._taps = [now - 2.0, now - 1.5, now - 1.0, now - 0.5]
        rig.tap()  # fifth tap at a steady 0.5 s spacing = 120 bpm
        assert 115 < rig.tempo_bpm < 125
        rig.set_tempo(120)
        rig.delay_div(0.25)
        assert abs(rig.chain.by_id["delay"].params["time"] - 500) < 1e-6
        assert rig.chain.by_id["delay"].enabled


def test_rig_hold_param_restores():
    with tempfile.TemporaryDirectory() as d:
        rig = Rig(Path(d) / "data", Path(d) / "media")
        f = rig.chain.by_id["filter"]
        f.set("cutoff", 5000)
        rig.pad_event(1, 6, True)            # LP 250 hold
        assert f.enabled and f.params["cutoff"] == 250 and f.choice("mode") == "lowpass"
        rig.pad_event(1, 6, False)
        assert not f.enabled and f.params["cutoff"] == 5000
        rig.pad_event(2, 5, True)            # tone preset: sine 100
        assert rig.engine.source.params["freq"] == 100 and rig.engine.source.params["wave"] == "sine"
        rig.pad_event(1, 4, True)            # lissajous 1:2
        s = rig.engine.source.params
        assert s["mode"] == "shape" and s["shape"] == "lissajous" and (s["a"], s["b"]) == (1, 2)
        colors = rig.pad_colors()
        assert colors[(1, 4)] == 53           # purple = active shape
        rig.panic()
        assert not any(fx.enabled for fx in rig.chain.effects)
