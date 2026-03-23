#!/usr/bin/env python3
"""
AART Remora™ Programmer — Test Suite
=====================================
Runs automated tests against mock_server.py without a browser.
Tests the WebSocket protocol layer, HTTP endpoints, and mock state.

Usage:
    # In one terminal:
    python3 mock_server.py

    # In another:
    python3 test_mock.py

Or run both together:
    python3 test_mock.py --start-server
"""

import argparse
import base64
import gzip
import hashlib
import http.client
import json
import os
import re
import socket
import struct
import subprocess
import sys
import time
import threading
import unittest
from pathlib import Path

# ── Minimal WS client (no third-party deps) ──────────────────────────────────

class WSClient:
    """Bare-bones synchronous WebSocket client for testing."""

    def __init__(self, host: str, port: int, path: str = "/ws", timeout: float = 3.0):
        self.host = host
        self.port = port
        self.path = path
        self.timeout = timeout
        self._sock = None
        self._rfile = None
        self._wfile = None

    def connect(self):
        s = socket.create_connection((self.host, self.port), timeout=self.timeout)
        self._sock = s
        self._rfile = s.makefile("rb")
        self._wfile = s.makefile("wb")
        self._handshake()

    def _handshake(self):
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            f"GET {self.path} HTTP/1.1\r\n"
            f"Host: {self.host}:{self.port}\r\n"
            f"Upgrade: websocket\r\n"
            f"Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            f"Sec-WebSocket-Version: 13\r\n"
            f"\r\n"
        )
        self._wfile.write(req.encode())
        self._wfile.flush()
        # Read response headers
        while True:
            line = self._rfile.readline()
            if line in (b"\r\n", b"\n", b""):
                break

    def send_text(self, text: str):
        payload = text.encode()
        self._send_frame(0x1, payload)

    def send_binary(self, data: bytes):
        self._send_frame(0x2, data)

    def _send_frame(self, opcode: int, payload: bytes):
        """Send a masked client frame."""
        length = len(payload)
        mask_key = os.urandom(4)
        masked = bytearray(payload)
        for i in range(len(masked)):
            masked[i] ^= mask_key[i % 4]
        header = bytes([0x80 | opcode])
        if length < 126:
            header += bytes([0x80 | length])
        elif length < 65536:
            header += bytes([0x80 | 126]) + struct.pack(">H", length)
        else:
            header += bytes([0x80 | 127]) + struct.pack(">Q", length)
        header += mask_key
        self._wfile.write(header + bytes(masked))
        self._wfile.flush()

    def recv_text(self) -> str:
        opcode, payload = self._recv_frame()
        return payload.decode("utf-8", errors="replace")

    def _recv_frame(self) -> tuple[int, bytes]:
        hdr = self._rfile.read(2)
        opcode = hdr[0] & 0x0f
        length = hdr[1] & 0x7f
        if length == 126:
            length = struct.unpack(">H", self._rfile.read(2))[0]
        elif length == 127:
            length = struct.unpack(">Q", self._rfile.read(8))[0]
        data = self._rfile.read(length)
        return opcode, data

    def close(self):
        try:
            self._send_frame(0x8, b"")
        except Exception:
            pass
        try:
            self._sock.close()
        except Exception:
            pass

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, *_):
        self.close()


# ── Helpers ───────────────────────────────────────────────────────────────────

HOST = "127.0.0.1"
PORT = 8080

def ws() -> WSClient:
    return WSClient(HOST, PORT)

def parse_response(raw: str) -> tuple[bool, list[str]]:
    """
    Parse a WS response string.
    Returns (ok, data_lines) where data_lines excludes the final OK/ERROR line.
    """
    lines = raw.split("\n")
    # drop trailing empty string
    while lines and lines[-1] == "":
        lines.pop()
    if not lines:
        return False, []
    status = lines.pop()
    ok = status == "OK"
    return ok, lines

def http_get(path: str) -> tuple[int, bytes, dict]:
    """Simple HTTP GET, returns (status, body_bytes, headers)."""
    conn = http.client.HTTPConnection(HOST, PORT, timeout=5)
    conn.request("GET", path)
    resp = conn.getresponse()
    body = resp.read()
    headers = dict(resp.getheaders())
    conn.close()
    return resp.status, body, headers


# ── Test cases ────────────────────────────────────────────────────────────────

class TestHTTPEndpoints(unittest.TestCase):

    def test_root_returns_html(self):
        status, body, headers = http_get("/")
        self.assertEqual(status, 200)
        ct = headers.get("Content-Type", "")
        self.assertIn("text/html", ct)

    def test_root_ws_url_patched(self):
        """The served HTML must NOT contain the ESP32 IP; it should point to localhost."""
        status, body, _ = http_get("/")
        encoding = _.get("Content-Encoding", "")
        html = gzip.decompress(body).decode() if encoding == "gzip" else body.decode()
        self.assertNotIn("192.168.4.1", html,
                         "ESP32 IP found in served HTML — WS URL patch failed")
        self.assertIn(f"/ws", html)  # WS URL present (host may be localhost or 127.0.0.1)

    def test_lang_en_json(self):
        status, body, headers = http_get("/?en")
        self.assertEqual(status, 200)
        ct = headers.get("Content-Type", "")
        self.assertIn("json", ct)
        encoding = headers.get("Content-Encoding", "")
        raw = gzip.decompress(body) if encoding == "gzip" else body
        data = json.loads(raw)
        self.assertIn("S00", data)
        self.assertIn("S02", data)

    def test_lang_de_json(self):
        status, body, headers = http_get("/?de")
        self.assertEqual(status, 200)
        encoding = headers.get("Content-Encoding", "")
        raw = gzip.decompress(body) if encoding == "gzip" else body
        data = json.loads(raw)
        self.assertIn("S00", data)

    def test_unknown_path_redirects(self):
        """Unknown paths redirect to / (captive portal behaviour, matching ESP32)."""
        status, _, _ = http_get("/nonexistent")
        self.assertIn(status, [301, 302, 308])

    def test_preset_missing_404(self):
        status, _, _ = http_get("/preset/does_not_exist.json")
        self.assertEqual(status, 404)


class TestWSBasicCommands(unittest.TestCase):

    def test_show_returns_params(self):
        with ws() as c:
            c.send_text("show\n")
            raw = c.recv_text()
        ok, lines = parse_response(raw)
        self.assertTrue(ok)
        keys = {l.split(":")[0].strip() for l in lines}
        for expected in ["damp", "timing", "freq_min", "freq_max",
                         "duty_min", "duty_max", "analog_min", "analog_max"]:
            self.assertIn(expected, keys, f"show missing key: {expected}")

    def test_get_known_param(self):
        with ws() as c:
            c.send_text("get freq_min\n")
            raw = c.recv_text()
        ok, lines = parse_response(raw)
        self.assertTrue(ok)
        self.assertTrue(any("freq_min" in l for l in lines))

    def test_set_and_get_roundtrip(self):
        with ws() as c:
            c.send_text("set timing 20\n")
            raw = c.recv_text()
            ok, _ = parse_response(raw)
            self.assertTrue(ok)

            c.send_text("get timing\n")
            raw2 = c.recv_text()
        ok2, lines = parse_response(raw2)
        self.assertTrue(ok2)
        self.assertTrue(any("timing: 20" in l for l in lines))

    def test_save(self):
        with ws() as c:
            c.send_text("save\n")
            raw = c.recv_text()
        ok, _ = parse_response(raw)
        self.assertTrue(ok)

    def test_reset(self):
        with ws() as c:
            c.send_text("reset\n")
            raw = c.recv_text()
        ok, _ = parse_response(raw)
        self.assertTrue(ok)

    def test_info_returns_telemetry(self):
        with ws() as c:
            c.send_text("info\n")
            raw = c.recv_text()
        ok, lines = parse_response(raw)
        self.assertTrue(ok)
        self.assertTrue(len(lines) > 0)

    def test_throt(self):
        with ws() as c:
            c.send_text("throt 1500\n")
            raw = c.recv_text()
        ok, lines = parse_response(raw)
        self.assertTrue(ok)
        self.assertTrue(any("1500" in l for l in lines))

    def test_unknown_command_returns_error(self):
        with ws() as c:
            c.send_text("totally_invalid_cmd\n")
            raw = c.recv_text()
        ok, _ = parse_response(raw)
        self.assertFalse(ok)


class TestWSBootloaderCommands(unittest.TestCase):

    def test_probe_returns_ok(self):
        with ws() as c:
            c.send_text("_probe\n")
            raw = c.recv_text()
        ok, lines = parse_response(raw)
        self.assertTrue(ok)

    def test_info_bootloader(self):
        with ws() as c:
            c.send_text("_info\n")
            raw = c.recv_text()
        ok, lines = parse_response(raw)
        self.assertTrue(ok)
        joined = "\n".join(lines)
        self.assertIn("1.04", joined)   # BL rev
        self.assertIn("1.06", joined)   # FW rev

    def test_setwrp(self):
        with ws() as c:
            c.send_text("_setwrp 0x33\n")
            raw = c.recv_text()
        ok, _ = parse_response(raw)
        self.assertTrue(ok)

    def test_firmware_upload_simulation(self):
        """Simulate a small firmware upload and check progress/result messages."""
        fake_fw = b"\xea\x32\x00\x00" + b"AT32F421\x00" + b"\xff" * (1024 - 14)
        size = len(fake_fw)

        with ws() as c:
            # Initiate update
            c.send_text(f"_update {size} 0 0\n")
            raw = c.recv_text()
            ok, _ = parse_response(raw)
            self.assertTrue(ok, "_update command rejected")

            # Send binary chunk
            c.send_binary(fake_fw)

            # Collect messages until _result arrives (with timeout)
            received_result = False
            received_status = False
            deadline = time.time() + 5.0
            while time.time() < deadline:
                try:
                    raw = c.recv_text()
                except socket.timeout:
                    break
                if "_status" in raw:
                    received_status = True
                if "_result 0" in raw:
                    received_result = True
                    break

        self.assertTrue(received_status, "No _status progress notification received")
        self.assertTrue(received_result, "No _result success notification received")


class TestWSWifiCommands(unittest.TestCase):

    def test_wifi_get(self):
        with ws() as c:
            c.send_text("_wifi_get\n")
            raw = c.recv_text()
        ok, lines = parse_response(raw)
        self.assertTrue(ok)
        self.assertTrue(any("_wifi_get" in l for l in lines))

    def test_wifi_set_and_get(self):
        with ws() as c:
            c.send_text("_wifi_set MyTestSSID\tSecret123\n")
            raw = c.recv_text()
            ok, _ = parse_response(raw)
            self.assertTrue(ok, "wifi_set failed")

            c.send_text("_wifi_get\n")
            raw2 = c.recv_text()
        ok2, lines = parse_response(raw2)
        self.assertTrue(ok2)
        joined = "\n".join(lines)
        self.assertIn("MyTestSSID", joined)
        self.assertIn("Secret123", joined)

    def test_wifi_set_no_password(self):
        with ws() as c:
            c.send_text("_wifi_set OpenNet\t\n")
            raw = c.recv_text()
        ok, _ = parse_response(raw)
        self.assertTrue(ok)

    def test_wifi_reset_to_default(self):
        with ws() as c:
            c.send_text("_wifi_set ESCape32-WiFi-Link\t\n")
            raw = c.recv_text()
        ok, _ = parse_response(raw)
        self.assertTrue(ok)


class TestWSPresetCommands(unittest.TestCase):

    def test_preset_save_and_retrieve(self):
        payload = json.dumps({"name": "Test preset", "timing": 18, "freq_min": 48})
        with ws() as c:
            c.send_text(f"_preset_save test_preset_01\t{payload}\n")
            raw = c.recv_text()
        ok, _ = parse_response(raw)
        self.assertTrue(ok, "preset_save failed")

        # Now retrieve via HTTP
        status, body, headers = http_get("/preset/test_preset_01.json")
        self.assertEqual(status, 200)
        enc = headers.get("Content-Encoding", "")
        raw_body = gzip.decompress(body) if enc == "gzip" else body
        data = json.loads(raw_body)
        self.assertEqual(data["timing"], 18)
        self.assertEqual(data["freq_min"], 48)

    def test_preset_missing_returns_404(self):
        status, _, _ = http_get("/preset/i_do_not_exist.json")
        self.assertEqual(status, 404)


class TestParamValidation(unittest.TestCase):
    """Check that MOCK_PARAMS covers all eCom-critical keys."""

    ECOM_KEYS = [
        "damp", "revdir", "timing", "freq_min", "freq_max",
        "duty_min", "duty_max", "duty_spup", "duty_ramp", "duty_rate",
        "analog_min", "analog_max",
    ]

    def test_all_ecom_keys_present_in_show(self):
        with ws() as c:
            c.send_text("show\n")
            raw = c.recv_text()
        _, lines = parse_response(raw)
        present = {l.split(":")[0].strip() for l in lines}
        for key in self.ECOM_KEYS:
            self.assertIn(key, present, f"eCom key missing from show: {key}")

    def test_set_all_ecom_keys(self):
        test_vals = {
            "damp": "1", "revdir": "0", "timing": "16",
            "freq_min": "48", "freq_max": "96",
            "duty_min": "100", "duty_max": "100",
            "duty_spup": "40", "duty_ramp": "60", "duty_rate": "50",
            "analog_min": "0", "analog_max": "1440",
        }
        with ws() as c:
            for key, val in test_vals.items():
                c.send_text(f"set {key} {val}\n")
                raw = c.recv_text()
                ok, _ = parse_response(raw)
                self.assertTrue(ok, f"set {key} {val} returned ERROR")


# ── Server launcher (optional) ────────────────────────────────────────────────

def wait_for_server(host: str, port: int, timeout: float = 10.0) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            s = socket.create_connection((host, port), timeout=0.5)
            s.close()
            return True
        except OSError:
            time.sleep(0.2)
    return False


def main():
    global HOST, PORT
    parser = argparse.ArgumentParser(description="AART mock server test suite")
    parser.add_argument("--host", default=HOST)
    parser.add_argument("--port", type=int, default=PORT)
    parser.add_argument("--start-server", action="store_true",
                        help="Launch mock_server.py as a subprocess before testing")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    HOST = args.host
    PORT = args.port

    server_proc = None
    if args.start_server:
        script = Path(__file__).parent / "mock_server.py"
        server_proc = subprocess.Popen(
            [sys.executable, str(script), "--host", args.host, "--port", str(args.port)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        print(f"Started mock_server.py (pid {server_proc.pid}), waiting for it to be ready...")
        if not wait_for_server(args.host, args.port):
            print("ERROR: server did not start in time")
            server_proc.terminate()
            sys.exit(1)
        print("Server ready.\n")
    else:
        # Check server is reachable
        if not wait_for_server(args.host, args.port, timeout=1.0):
            print(f"ERROR: No server at {args.host}:{args.port}")
            print("Start mock_server.py first, or pass --start-server")
            sys.exit(1)

    # Patch the module-level HOST/PORT used by helper functions
    import test_mock as self_mod
    self_mod.HOST = args.host
    self_mod.PORT = args.port

    verbosity = 2 if args.verbose else 1
    loader = unittest.TestLoader()
    suite  = loader.loadTestsFromModule(sys.modules[__name__])
    runner = unittest.TextTestRunner(verbosity=verbosity)
    result = runner.run(suite)

    if server_proc:
        server_proc.terminate()
        server_proc.wait()

    sys.exit(0 if result.wasSuccessful() else 1)


if __name__ == "__main__":
    main()
