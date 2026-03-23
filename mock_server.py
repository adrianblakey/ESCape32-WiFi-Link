#!/usr/bin/env python3
"""
AART Remora(tm) Programmer - Desktop Mock Server
=================================================
Runs a local HTTP + WebSocket server that serves root.html and simulates
the ESCape32 Wi-Fi Link firmware responses, so you can develop and test
the web UI on any Mac or Linux machine without an ESP32.

Requirements: Python 3.7+  (stdlib only, no pip installs needed)

Usage:
    python3 mock_server.py [--port 8080] [--html path/to/root.html]

Then open http://localhost:8080 in your browser.
Press Ctrl-C to stop.

The mock ESC state is editable at the top of MOCK_STATE below.
All 'set' commands update it in memory; 'save'/'reset' are acknowledged.
Wi-Fi and preset operations are backed by a mock_presets/ directory.
"""

import argparse
import base64
import hashlib
import http.server
import json
import logging
import os
import re
import struct
import sys
import time
from pathlib import Path
from socketserver import TCPServer

# ──────────────────────────────────────────────────────────────────────────────
# Mock ESC state — edit to simulate different board configurations
# ──────────────────────────────────────────────────────────────────────────────
MOCK_STATE = {
    "arm":         0,
    "damp":        1,
    "revdir":      0,
    "brushed":     0,
    "timing":      14,
    "sine_range":  25,
    "sine_power":  25,
    "freq_min":    48,
    "freq_max":    48,
    "duty_min":    100,
    "duty_max":    100,
    "duty_spup":   30,
    "duty_ramp":   50,
    "duty_rate":   35,
    "duty_drag":   0,
    "duty_lock":   0,
    "throt_mode":  0,
    "throt_rev":   0,
    "throt_brk":   25,
    "throt_set":   0,
    "throt_ztc":   0,
    "throt_cal":   1,
    "throt_min":   0,
    "throt_mid":   1000,
    "throt_max":   2000,
    "analog_min":  0,
    "analog_max":  1440,
    "input_mode":  1,
    "input_ch1":   0,
    "input_ch2":   1,
    "telem_mode":  0,
    "telem_phid":  1,
    "telem_poles": 14,
    "prot_stall":  0,
    "prot_temp":   80,
    "prot_sens":   0,
    "prot_volt":   33,
    "prot_cells":  0,
    "prot_curr":   0,
    "music":       "",
    "volume":      25,
    "beacon":      25,
    "bec":         0,
    "led":         0,
}

MOCK_INFO = {
    "bootloader_rev": "1.5",
    "io_pin":         1,
    "mcu":            "AT32F421",
    "fw_version":     "0.99",
    "fw_target":      "Remora2",
}

DEFAULT_WIFI = {"ssid": "ESCape32-WiFi-Link", "pass": ""}

# ──────────────────────────────────────────────────────────────────────────────
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s  %(levelname)-7s  %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("mock")

SCRIPT_DIR = Path(__file__).parent
PRESET_DIR = SCRIPT_DIR / "mock_presets"
PRESET_DIR.mkdir(exist_ok=True)
WIFI_FILE  = PRESET_DIR / "wifi.json"
LANG_FILES = {}
HTML_PATH  = None


def load_lang_files(html_dir):
    for path in sorted(html_dir.glob("root_*.json")):
        m = re.match(r"root_(\w+)\.json", path.name)
        if m:
            LANG_FILES[m.group(1)] = path.read_text(encoding="utf-8")
            log.info("Loaded language: %s", path.name)


def get_wifi():
    if WIFI_FILE.exists():
        try:
            return json.loads(WIFI_FILE.read_text())
        except Exception:
            pass
    return dict(DEFAULT_WIFI)


def set_wifi(ssid, password):
    WIFI_FILE.write_text(json.dumps({"ssid": ssid, "pass": password}, indent=2))


# ──────────────────────────────────────────────────────────────────────────────
# WebSocket (RFC 6455, stdlib only)
# ──────────────────────────────────────────────────────────────────────────────
WS_MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def ws_accept(key):
    digest = hashlib.sha1((key + WS_MAGIC).encode()).digest()
    return base64.b64encode(digest).decode()


def ws_handshake_response(key):
    return (
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: {}\r\n"
        "\r\n"
    ).format(ws_accept(key)).encode()


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("closed")
        buf += chunk
    return buf


def ws_read_frame(sock):
    """Returns (opcode, payload_bytes) or raises."""
    h = recv_exact(sock, 2)
    opcode = h[0] & 0x0f
    masked = (h[1] & 0x80) != 0
    length = h[1] & 0x7f
    if length == 126:
        length = struct.unpack(">H", recv_exact(sock, 2))[0]
    elif length == 127:
        length = struct.unpack(">Q", recv_exact(sock, 8))[0]
    mask_key = recv_exact(sock, 4) if masked else b""
    data = bytearray(recv_exact(sock, length))
    if masked:
        for i in range(len(data)):
            data[i] ^= mask_key[i % 4]
    return opcode, bytes(data)


def ws_frame(payload, opcode=1):
    if isinstance(payload, str):
        payload = payload.encode()
    n = len(payload)
    if n <= 125:
        hdr = bytes([0x80 | opcode, n])
    elif n <= 65535:
        hdr = bytes([0x80 | opcode, 126]) + struct.pack(">H", n)
    else:
        hdr = bytes([0x80 | opcode, 127]) + struct.pack(">Q", n)
    return hdr + payload


def ws_send(sock, text):
    try:
        sock.sendall(ws_frame(text))
        log.debug("WS <-- %r", text[:100])
    except Exception as exc:
        log.warning("ws_send: %s", exc)


# ──────────────────────────────────────────────────────────────────────────────
# ESC command simulator
# ──────────────────────────────────────────────────────────────────────────────
def process_command(raw):
    cmd = raw.strip()
    log.info("CMD --> %r", cmd[:100])

    if cmd == "show":
        body = "\n".join("{}: {}".format(k, v) for k, v in MOCK_STATE.items())
        return "show\n{}\nOK\n".format(body)

    if cmd == "info":
        return "info\n12345 rpm  0.0A  3.7V\nOK\n"

    m = re.match(r"^get (\w+)$", cmd)
    if m:
        k = m.group(1)
        return "get {}\n{}: {}\nOK\n".format(k, k, MOCK_STATE.get(k, ""))

    m = re.match(r"^set (\w+) (.+)$", cmd)
    if m:
        k, v = m.group(1), m.group(2)
        if k in MOCK_STATE:
            old = MOCK_STATE[k]
            try:
                MOCK_STATE[k] = int(v) if isinstance(old, int) else v
            except ValueError:
                MOCK_STATE[k] = v
            log.info("  SET %s = %r  (was %r)", k, MOCK_STATE[k], old)
        return "set {}\n{}: {}\nOK\n".format(k, k, v)

    if cmd == "save":
        log.info("  SAVE acknowledged")
        return "save\nOK\n"

    if cmd == "reset":
        MOCK_STATE.update({"timing": 0, "damp": 0, "revdir": 0})
        log.info("  RESET (partial defaults restored)")
        return "reset\nOK\n"

    m = re.match(r"^throt (\d+)$", cmd)
    if m:
        return "throt {}\nOK\n".format(m.group(1))

    if cmd.startswith("play "):
        log.info("  PLAY %s", cmd[5:60])
        return "play\nOK\n"

    if cmd == "_probe":
        return "_probe 1\nOK\n"

    if cmd == "_info":
        mi = MOCK_INFO
        return "_info\n{} {} {}\n{}\n{}\nOK\n".format(
            mi["bootloader_rev"], mi["io_pin"], mi["mcu"],
            mi["fw_version"], mi["fw_target"]
        )

    m = re.match(r"^_setwrp (.+)$", cmd)
    if m:
        log.info("  SETWRP val=%s", m.group(1))
        return "_setwrp\nOK\n"

    m = re.match(r"^_update (\d+) (\d+) (\w+)$", cmd)
    if m:
        log.info("  UPDATE size=%s boot=%s wrp=%s", *m.groups())
        return "_update\nOK\n"

    if cmd == "_wifi_get":
        cfg = get_wifi()
        return "_wifi_get\n{}\t{}\nOK\n".format(cfg["ssid"], cfg["pass"])

    m = re.match(r"^_wifi_set (.+)$", cmd)
    if m:
        parts = m.group(1).split("\t", 1)
        ssid = parts[0].strip()
        pw   = parts[1].strip() if len(parts) > 1 else ""
        set_wifi(ssid, pw)
        log.info("  WIFI ssid=%r pass=%r saved", ssid, pw)
        return "_wifi_set\nOK\n"

    m = re.match(r"^_preset_save (\S+)\t(.+)$", cmd, re.DOTALL)
    if m:
        slug    = m.group(1)[:32]
        payload = m.group(2)
        out = PRESET_DIR / "{}.json".format(slug)
        out.write_text(payload, encoding="utf-8")
        log.info("  PRESET saved: %s", out)
        return "_preset_save\nOK\n"

    log.warning("  UNHANDLED: %r", cmd)
    tag = cmd.split()[0] if cmd else "unknown"
    return "{}\nERROR\n".format(tag)


# ──────────────────────────────────────────────────────────────────────────────
# HTTP handler
# ──────────────────────────────────────────────────────────────────────────────
class Handler(http.server.BaseHTTPRequestHandler):

    def log_message(self, fmt, *args):
        log.info("HTTP  " + fmt, *args)

    def do_GET(self):
        parts = self.path.split("?", 1)
        route = parts[0]
        query = parts[1] if len(parts) > 1 else ""

        # WebSocket upgrade
        if route == "/ws":
            if "websocket" in self.headers.get("Upgrade", "").lower():
                self._ws_upgrade()
            else:
                self._text(400, "Expected WebSocket upgrade")
            return

        # Language files: /?en  /?de  etc.
        if route == "/" and query in LANG_FILES:
            self._raw(200, "application/json", LANG_FILES[query].encode())
            return

        # Preset JSON: /preset/<slug>.json
        m = re.match(r"^/preset/([A-Za-z0-9_\-]+)\.json$", route)
        if m:
            p = PRESET_DIR / "{}.json".format(m.group(1)[:32])
            if p.exists():
                self._raw(200, "application/json", p.read_bytes())
            else:
                self._text(404, "Not found")
            return

        # Root HTML
        if route in ("/", "/index.html"):
            self._raw(200, "text/html; charset=utf-8", self._patched_html())
            return

        # Fallback redirect
        self.send_response(302)
        self.send_header("Location", "/")
        self.end_headers()

    def _patched_html(self):
        port = self.server.server_address[1]
        text = HTML_PATH.read_text(encoding="utf-8")
        text = text.replace("ws://192.168.4.1/ws",
                            "ws://localhost:{}/ws".format(port))
        if "@LANG_OPTS@" in text:
            opts = "".join(
                '<option value="{}">{}</option>'.format(lang, lang.upper())
                for lang in sorted(LANG_FILES.keys())
            ) or '<option value="en">EN</option>'
            text = text.replace("@LANG_OPTS@", opts)
        text = text.replace("@PROJECT_VER@", "mock-dev")
        return text.encode()

    def _raw(self, code, ctype, body):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(body)

    def _text(self, code, msg):
        self._raw(code, "text/plain", msg.encode())

    def _ws_upgrade(self):
        key = self.headers.get("Sec-WebSocket-Key", "")
        self.wfile.write(ws_handshake_response(key))
        self.wfile.flush()
        log.info("WS    connected: %s", self.client_address)
        sock = self.connection
        sock.settimeout(None)
        try:
            while True:
                opcode, payload = ws_read_frame(sock)
                if opcode == 8:   # close
                    log.info("WS    close frame received")
                    break
                if opcode == 9:   # ping
                    sock.sendall(ws_frame(payload, opcode=10))
                    continue
                if opcode == 2:   # binary (firmware chunk)
                    log.info("WS    binary %d bytes (fw update)", len(payload))
                    ws_send(sock, "_status 50\nOK\n")
                    time.sleep(0.05)
                    ws_send(sock, "_status 100\nOK\n")
                    ws_send(sock, "_result 0\nOK\n")
                    continue
                if opcode == 1:   # text command
                    resp = process_command(payload.decode(errors="replace"))
                    ws_send(sock, resp)
        except Exception as exc:
            log.info("WS    disconnected: %s", exc)
        log.info("WS    closed")


# ──────────────────────────────────────────────────────────────────────────────
# main
# ──────────────────────────────────────────────────────────────────────────────
def main():
    global HTML_PATH

    ap = argparse.ArgumentParser(
        description="AART Remora(tm) Programmer - mock server"
    )
    ap.add_argument("--port",  type=int, default=8080,
                    help="HTTP/WS port (default 8080)")
    ap.add_argument("--html",  default=None,
                    help="Path to root.html (default: same dir as this script)")
    ap.add_argument("--debug", action="store_true",
                    help="Verbose debug logging")
    args = ap.parse_args()

    if args.debug:
        logging.getLogger().setLevel(logging.DEBUG)

    HTML_PATH = Path(args.html) if args.html else SCRIPT_DIR / "main" / "root.html"
    if not HTML_PATH.exists():
        sys.exit(
            "ERROR: root.html not found at {}\n"
            "       Place root.html next to mock_server.py, or use --html <path>".format(HTML_PATH)
        )

    load_lang_files(HTML_PATH.parent)

    TCPServer.allow_reuse_address = True
    server = TCPServer(("", args.port), Handler)

    url = "http://localhost:{}".format(args.port)
    bar = "=" * 56
    log.info(bar)
    log.info("  AART Remora(tm) Programmer  --  Mock Server")
    log.info(bar)
    log.info("  HTML:      %s", HTML_PATH)
    log.info("  URL:       %s", url)
    log.info("  Presets:   %s/", PRESET_DIR)
    log.info("  Languages: %s",
             ", ".join(sorted(LANG_FILES.keys())) or "(none found)")
    log.info(bar)
    log.info("  Open %s in your browser", url)
    log.info("  Press Ctrl-C to stop")
    log.info("")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log.info("Stopped.")


if __name__ == "__main__":
    main()
