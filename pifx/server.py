"""HTTP + WebSocket server for the pifx web UI, standard library only.

Routes
  GET  /                 the control page (web/index.html)
  GET  /api/state        full rig state (JSON)
  GET  /api/status       HAT detection report, engine status, Launchpad status
  GET  /api/devices      audio + MIDI devices
  POST /api/cmd          one command, same JSON as over the WebSocket
  WS   /ws               bidirectional control; see rig.Rig.command for ops

WebSocket text frames carry JSON. Binary frames from the server carry meter
data (see `_meter_frame`), ~15 times a second.
"""
from __future__ import annotations

import base64
import hashlib
import json
import logging
import queue
import socket
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Optional

import numpy as np

from .rig import Rig

log = logging.getLogger("pifx.server")
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
WEB_DIR = Path(__file__).resolve().parent.parent / "web"
METER_HZ = 15
SCOPE_POINTS = 1024
SPECTRUM_BANDS = 64


# --------------------------------------------------------------------------
# WebSocket (RFC 6455) over the handler's socket
# --------------------------------------------------------------------------
class WebSocket:
    def __init__(self, sock: socket.socket, rfile=None):
        self.sock = sock
        self.rfile = rfile          # the handler's buffered reader (may already hold bytes)
        self.q: queue.Queue = queue.Queue(maxsize=32)
        self.open = True
        self._sender = threading.Thread(target=self._send_loop, daemon=True)
        self._sender.start()

    # -- outgoing ----------------------------------------------------------
    def send_json(self, obj: dict) -> None:
        self._enqueue((0x1, json.dumps(obj, separators=(",", ":")).encode()), important=True)

    def send_binary(self, data: bytes) -> None:
        self._enqueue((0x2, data), important=False)

    def _enqueue(self, item, important: bool) -> None:
        if not self.open:
            return
        try:
            self.q.put_nowait(item)
        except queue.Full:
            if important:
                try:                      # drop something older (meter data) to make room
                    self.q.get_nowait()
                    self.q.put_nowait(item)
                except (queue.Empty, queue.Full):
                    pass

    def _send_loop(self) -> None:
        while self.open:
            try:
                item = self.q.get(timeout=1.0)
            except queue.Empty:
                continue
            if item is None:
                break
            opcode, payload = item
            try:
                self.sock.sendall(self._frame(opcode, payload))
            except OSError:
                self.open = False
        self.open = False

    @staticmethod
    def _frame(opcode: int, payload: bytes) -> bytes:
        n = len(payload)
        head = bytes([0x80 | opcode])
        if n < 126:
            head += bytes([n])
        elif n < 65536:
            head += bytes([126]) + struct.pack(">H", n)
        else:
            head += bytes([127]) + struct.pack(">Q", n)
        return head + payload

    # -- incoming ----------------------------------------------------------
    def _recv_exact(self, n: int) -> bytes:
        if n == 0:
            return b""
        if self.rfile is not None:
            buf = self.rfile.read(n)
            if len(buf) < n:
                raise ConnectionError("closed")
            return buf
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("closed")
            buf += chunk
        return buf

    def recv(self) -> Optional[tuple]:
        """Return (opcode, payload) for the next complete message, None when closed."""
        message = b""
        msg_opcode = None
        while True:
            try:
                b1, b2 = self._recv_exact(2)
            except (OSError, ConnectionError):
                return None
            fin, opcode = b1 & 0x80, b1 & 0x0F
            masked, length = b2 & 0x80, b2 & 0x7F
            if length == 126:
                length = struct.unpack(">H", self._recv_exact(2))[0]
            elif length == 127:
                length = struct.unpack(">Q", self._recv_exact(8))[0]
            if length > 1 << 20:
                return None
            mask = self._recv_exact(4) if masked else None
            payload = self._recv_exact(length)
            if mask:
                payload = _unmask(payload, mask)
            if opcode == 0x8:
                return None
            if opcode == 0x9:                 # ping -> pong
                self._enqueue((0xA, payload), important=True)
                continue
            if opcode == 0xA:
                continue
            if opcode in (0x1, 0x2):
                msg_opcode = opcode
                message = payload
            elif opcode == 0x0:
                message += payload
            if fin:
                return msg_opcode, message

    def close(self) -> None:
        if self.open:
            try:
                self.sock.sendall(self._frame(0x8, struct.pack(">H", 1000)))
            except OSError:
                pass
        self.open = False
        try:
            self.q.put_nowait(None)
        except queue.Full:
            pass


def _unmask(payload: bytes, mask: bytes) -> bytes:
    if len(payload) > 64:
        arr = np.frombuffer(payload, dtype=np.uint8)
        m = np.frombuffer((mask * (len(payload) // 4 + 1))[: len(payload)], dtype=np.uint8)
        return (arr ^ m).tobytes()
    return bytes(b ^ mask[i % 4] for i, b in enumerate(payload))


def ws_accept_key(client_key: str) -> str:
    return base64.b64encode(hashlib.sha1((client_key + WS_GUID).encode()).digest()).decode()


# --------------------------------------------------------------------------
# HTTP handler
# --------------------------------------------------------------------------
class Handler(BaseHTTPRequestHandler):
    server: "PifxServer"
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # quieter than the default
        log.debug("%s - %s", self.address_string(), fmt % args)

    # -- helpers -------------------------------------------------------------
    def _json(self, obj, code: int = 200) -> None:
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _file(self, path: Path, ctype: str) -> None:
        try:
            body = path.read_bytes()
        except OSError:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    # -- routes ----------------------------------------------------------------
    def do_GET(self):  # noqa: N802
        path = self.path.split("?", 1)[0]
        rig = self.server.rig
        if path == "/ws":
            self._websocket()
            return
        if path in ("/", "/index.html"):
            self._file(WEB_DIR / "index.html", "text/html; charset=utf-8")
        elif path == "/favicon.ico":
            self.send_response(204)
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif path == "/api/state":
            self._json(rig.state())
        elif path == "/api/status":
            self._json({**rig.status(), "launchpad": self.server.launchpad_status()})
        elif path == "/api/devices":
            from .engine import list_devices
            midi = []
            try:
                import mido
                midi = mido.get_input_names()
            except Exception as e:  # noqa: BLE001
                midi = [f"(mido unavailable: {e})"]
            self._json({"audio": list_devices(), "midi": midi})
        else:
            self.send_error(404)

    def do_POST(self):  # noqa: N802
        path = self.path.split("?", 1)[0]
        if path != "/api/cmd":
            self.send_error(404)
            return
        n = int(self.headers.get("Content-Length") or 0)
        try:
            msg = json.loads(self.rfile.read(n) or b"{}")
            reply = self.server.rig.command(msg)
            self._json(reply or {"ok": True})
        except Exception as e:  # noqa: BLE001
            self._json({"ok": False, "error": str(e)}, 400)

    # -- websocket -------------------------------------------------------------
    def _websocket(self) -> None:
        key = self.headers.get("Sec-WebSocket-Key")
        if self.headers.get("Upgrade", "").lower() != "websocket" or not key:
            self.send_error(400, "websocket upgrade expected")
            return
        self.send_response(101, "Switching Protocols")
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", ws_accept_key(key))
        self.end_headers()
        self.wfile.flush()
        self.close_connection = True
        self.connection.settimeout(None)
        ws = WebSocket(self.connection, self.rfile)
        self.server.add_client(ws)
        rig = self.server.rig
        try:
            ws.send_json({"type": "hello", "version": self.server.version})
            ws.send_json({"type": "state", "state": rig.state()})
            ws.send_json({"type": "status", **rig.status(), "launchpad": self.server.launchpad_status()})
            while ws.open:
                msg = ws.recv()
                if msg is None:
                    break
                opcode, payload = msg
                if opcode != 0x1:
                    continue
                try:
                    data = json.loads(payload)
                    reply = rig.command(data)
                    if reply:
                        ws.send_json(reply)
                except Exception as e:  # noqa: BLE001
                    ws.send_json({"type": "error", "error": str(e), "cmd": payload.decode(errors="replace")[:200]})
        finally:
            self.server.remove_client(ws)
            ws.close()


# --------------------------------------------------------------------------
# the server
# --------------------------------------------------------------------------
class PifxServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, addr, rig: Rig, launchpad=None, version: str = "dev"):
        super().__init__(addr, Handler)
        self.rig = rig
        self.launchpad = launchpad
        self.version = version
        self.clients: set[WebSocket] = set()
        self._clients_lock = threading.Lock()
        self._stop = threading.Event()
        rig.on_change(self._on_rig_change)
        self._meter_thread = threading.Thread(target=self._meter_loop, daemon=True)
        self._meter_thread.start()

    def launchpad_status(self) -> dict:
        return self.launchpad.status() if self.launchpad else {"connected": False, "available": False,
                                                                "error": "disabled"}

    # -- clients -----------------------------------------------------------------
    def add_client(self, ws: WebSocket) -> None:
        with self._clients_lock:
            self.clients.add(ws)

    def remove_client(self, ws: WebSocket) -> None:
        with self._clients_lock:
            self.clients.discard(ws)

    def broadcast_json(self, obj: dict) -> None:
        with self._clients_lock:
            clients = list(self.clients)
        for ws in clients:
            ws.send_json(obj)

    def broadcast_binary(self, data: bytes) -> None:
        with self._clients_lock:
            clients = list(self.clients)
        for ws in clients:
            ws.send_binary(data)

    # -- rig events ----------------------------------------------------------------
    def _on_rig_change(self, what: str, payload: dict) -> None:
        if what == "status":
            self.broadcast_json({"type": "status", **self.rig.status(), "launchpad": self.launchpad_status()})
        else:
            self.broadcast_json({"type": "state", "state": self.rig.state()})
        if self.launchpad:
            self.launchpad.refresh()

    # -- meters ----------------------------------------------------------------------
    def _meter_frame(self) -> bytes:
        e = self.rig.engine
        snap = e.scope_snapshot(SCOPE_POINTS)                       # (n, 4) in/out L/R
        spec = e.spectrum(SPECTRUM_BANDS)
        head = struct.pack("<BBHHffffffII", 1, 4, SCOPE_POINTS, SPECTRUM_BANDS,
                           float(e.peak[0]), float(e.peak[1]), float(e.rms[0]), float(e.rms[1]),
                           float(self.rig.chain.limiter.reduction_db), float(e.load),
                           int(e.xruns), int(time.time() * 1000) & 0xFFFFFFFF)
        scope_i16 = np.clip(snap * 32767, -32768, 32767).astype("<i2").tobytes()
        spec_i16 = np.clip(spec * 100, -32768, 32767).astype("<i2").tobytes()
        return head + scope_i16 + spec_i16

    def _meter_loop(self) -> None:
        period = 1.0 / METER_HZ
        last_status = 0.0
        while not self._stop.is_set():
            t0 = time.perf_counter()
            if self.clients:
                try:
                    self.broadcast_binary(self._meter_frame())
                except Exception:  # noqa: BLE001
                    log.exception("meter frame")
                if t0 - last_status > 2.0:
                    last_status = t0
                    self.broadcast_json({"type": "tick", "engine": self.rig.engine.status(),
                                         "launchpad": self.launchpad_status(),
                                         "source": self.rig.engine.source.to_dict()})
            dt = time.perf_counter() - t0
            self._stop.wait(max(0.01, period - dt))

    def shutdown(self) -> None:
        self._stop.set()
        with self._clients_lock:
            clients = list(self.clients)
        for ws in clients:
            ws.close()
        super().shutdown()


def serve(rig: Rig, host: str, port: int, launchpad=None, version: str = "dev") -> PifxServer:
    srv = PifxServer((host, port), rig, launchpad, version)
    t = threading.Thread(target=srv.serve_forever, name="pifx-http", daemon=True)
    t.start()
    log.info("web UI on http://%s:%d/", host if host != "0.0.0.0" else _local_ip(), port)
    return srv


def _local_ip() -> str:
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("10.255.255.255", 1))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except OSError:
        return "localhost"
