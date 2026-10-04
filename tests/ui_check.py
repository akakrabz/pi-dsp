#!/usr/bin/env python3
"""Headless-browser check of the web UI against a --sim server (needs playwright).

Starts the server, loads the page, exercises the main controls over the real
WebSocket, screenshots the result and fails on any console error.
Usage: python3 tests/ui_check.py [outdir]
"""
import json
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "tests" / "shots"
OUT.mkdir(parents=True, exist_ok=True)
PORT = 8791


def get(path):
    with urllib.request.urlopen(f"http://127.0.0.1:{PORT}{path}", timeout=5) as r:
        return json.loads(r.read())


def main() -> int:
    srv = subprocess.Popen([sys.executable, "-m", "pifx", "--sim", "--no-launchpad", "--port", str(PORT),
                            "--data", str(OUT / "data"), "--media", str(OUT / "media")],
                           cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        for _ in range(50):
            try:
                get("/api/state")
                break
            except Exception:  # noqa: BLE001
                time.sleep(0.2)
        else:
            print(srv.stdout.read())
            raise SystemExit("server did not start")

        from playwright.sync_api import sync_playwright
        errors = []
        with sync_playwright() as p:
            browser = p.chromium.launch()
            page = browser.new_page(viewport={"width": 1440, "height": 1100})
            page.on("console", lambda m: errors.append(m.text) if m.type == "error" else None)
            page.on("pageerror", lambda e: errors.append(str(e)))
            page.goto(f"http://127.0.0.1:{PORT}/")
            page.wait_for_selector(".fxp", timeout=5000)
            page.wait_for_timeout(1200)
            assert page.locator("#ws-txt").inner_text() == "connected", "websocket did not connect"

            # 1. toggle delay via its footswitch, set a param via slider keyboard
            page.locator(".fxp").nth(4).locator("button.foot").click()
            page.wait_for_timeout(300)
            st = get("/api/state")
            assert [f for f in st["chain"]["fx"] if f["id"] == "delay"][0]["enabled"], "delay toggle failed"

            # 2. virtual Launchpad: press 'drive' toggle pad (x=2,y=7) and hold stutter (0,6)
            pads = page.locator(".pad:not(.blank)")
            # grid is built top row (y=8) first; pad for (x,y) index: row (8-y)*9 + x, minus the blank at (8,8)
            def pad_index(x, y):
                i = (8 - y) * 9 + x
                return i - 1 if i > 8 else i
            pads.nth(pad_index(2, 7)).dispatch_event("pointerdown", {"pointerId": 1})
            pads.nth(pad_index(2, 7)).dispatch_event("pointerup", {"pointerId": 1})
            page.wait_for_timeout(300)
            st = get("/api/state")
            assert [f for f in st["chain"]["fx"] if f["id"] == "drive"][0]["enabled"], "pad toggle failed"
            pads.nth(pad_index(0, 6)).dispatch_event("pointerdown", {"pointerId": 1})
            page.wait_for_timeout(200)
            assert [f for f in get("/api/state")["chain"]["fx"] if f["id"] == "stutter"][0]["enabled"], "hold press failed"
            pads.nth(pad_index(0, 6)).dispatch_event("pointerup", {"pointerId": 1})
            page.wait_for_timeout(200)
            assert not [f for f in get("/api/state")["chain"]["fx"] if f["id"] == "stutter"][0]["enabled"], "hold release failed"

            # 3. source: X-Y shape, scope X-Y tab
            page.locator("#src-kind button[data-k=shape]").click()
            page.wait_for_timeout(300)
            assert get("/api/state")["source"]["mode"] == "shape"
            page.locator("#shapes button", has_text="lissajous").click()
            page.locator("#scope-mode button[data-m=xy]").click()
            page.wait_for_timeout(900)
            page.screenshot(path=str(OUT / "ui-xy.png"), full_page=True)

            # 4. transfer curve with drive on, sine source
            page.locator("#src-kind button[data-k=tone]").click()
            page.wait_for_timeout(300)
            page.locator("#scope-mode button[data-m=transfer]").click()
            page.wait_for_timeout(900)
            page.screenshot(path=str(OUT / "ui-transfer.png"), clip={"x": 300, "y": 60, "width": 840, "height": 420})

            # 5. presets: save slot 3 then load
            page.locator("#slots .slot").nth(2).locator("button.save").click()
            page.wait_for_timeout(300)
            assert "slot3" in get("/api/state")["presets"]
            page.locator("#slots .slot").nth(2).locator("button.load").click()
            page.wait_for_timeout(300)
            assert get("/api/state")["preset"] == 3

            page.locator("#scope-mode button[data-m=wave]").click()
            page.wait_for_timeout(600)
            page.screenshot(path=str(OUT / "ui-desktop.png"), full_page=True)
            page.locator("#scope-mode button[data-m=spec]").click()
            page.wait_for_timeout(600)
            page.screenshot(path=str(OUT / "ui-spectrum.png"), clip={"x": 300, "y": 60, "width": 840, "height": 420})

            # phone layout
            phone = browser.new_page(viewport={"width": 390, "height": 844}, device_scale_factor=2)
            phone.on("pageerror", lambda e: errors.append(str(e)))
            phone.goto(f"http://127.0.0.1:{PORT}/")
            phone.wait_for_selector(".fxp", timeout=5000)
            phone.wait_for_timeout(800)
            width = phone.evaluate("document.documentElement.scrollWidth")
            assert width <= 390, f"horizontal overflow on phone: {width}"
            phone.screenshot(path=str(OUT / "ui-phone.png"), full_page=True)
            browser.close()
        if errors:
            print("console errors:\n  " + "\n  ".join(errors))
            return 1
        print(f"UI check passed; screenshots in {OUT}")
        return 0
    finally:
        srv.terminate()
        try:
            out, _ = srv.communicate(timeout=5)
            if "Traceback" in out or "ERROR" in out:
                print("server log:\n" + out[-3000:])
        except subprocess.TimeoutExpired:
            srv.kill()


if __name__ == "__main__":
    sys.exit(main())
