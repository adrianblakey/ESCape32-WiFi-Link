# AART Remora™ Programmer

Web-based wireless programmer for the [Remora™ electronic commutator (eCom)](https://aart.dev) — a brushless motor speed controller for slot car racing. Runs on an ESP32-S2 Wi-Fi module and lets you configure all ESCape32 firmware parameters from any browser, with no app install required.

---

## File Structure

```
aart-remora-programmer/
│
├── README.md                     ← This file
├── CMakeLists.txt                ← ESP-IDF top-level project file
├── sdkconfig.defaults            ← ESP32-S2 pin defaults (UART, LED)
├── mock_server.py                ← Python mock server (no ESP32 needed)
├── test_mock.py                  ← Automated Python test suite
│
├── main/                         ← ESP-IDF component (application source)
│   ├── CMakeLists.txt            ← Component registration; embeds root*.*
│   ├── main.c                    ← HTTP server, WebSocket, DNS, NVS
│   ├── build_defs.h.in           ← CMake template → build_defs.h
│   ├── Kconfig.projbuild         ← menuconfig: UART pins, LED pin
│   ├── idf_component.yml         ← Component dependencies (mdns)
│   ├── root.html                 ← Single-page web UI (5 tabs)
│   ├── root_en.json              ← UI strings – English
│   ├── root_de.json              ← UI strings – German
│   ├── root_fr.json              ← UI strings – French
│   ├── root_it.json              ← UI strings – Italian
│   ├── root_ru.json              ← UI strings – Russian
│   └── root_zh.json              ← UI strings – Chinese (Simplified)
│
└── linux_build/                  ← Native Linux/macOS build + tests
    ├── Makefile.linux            ← make test / run / clean
    ├── main.c                    ← main/main.c with Linux build guards
    ├── linux_main.c              ← Entry point; --port flag
    ├── linux_httpd.c             ← POSIX HTTP/1.1 + WebSocket server
    ├── esp_idf_shims.h           ← ESP-IDF header replacements
    ├── mock_uart.h               ← Pipe-backed ESC simulator
    ├── build_defs.h              ← Includes embed_data.h + LANG_LIST
    ├── embed_files.py            ← Gzips main/root.html + root_*.json
    └── test_ws.c                 ← 24 end-to-end C tests
```

---

## Quick Start

### Option 1 — Python mock server (fastest, no build needed)

```bash
# Serve the UI and simulate ESC responses
python3 mock_server.py

# Open in browser
open http://localhost:8080        # macOS
xdg-open http://localhost:8080    # Linux

# Different port
python3 mock_server.py --port 9090

# Run automated tests (starts its own server)
python3 test_mock.py --start-server
```

### Option 2 — Native Linux/macOS binary (tests the actual C code)

```bash
cd linux_build
make -f Makefile.linux check-deps   # verify gcc + python3
make -f Makefile.linux test         # build + run 24 tests → 24/24 green
make -f Makefile.linux run          # start server on :8080
make -f Makefile.linux PORT=9090 run
make -f Makefile.linux clean
```

### Option 3 — Flash to ESP32-S2 (production)

```bash
# Install ESP-IDF 5.x, then from the project root:
idf.py set-target esp32s2
idf.py build flash monitor

# sdkconfig.defaults sets correct pins automatically.
# Override with:  idf.py menuconfig → ESCape32-WiFi-Link configuration

# Connect to Wi-Fi AP:  ESCape32-WiFi-Link
# Open browser:         http://192.168.4.1
#                  or:  http://escape32.local
```

---

## How It Works

```
Browser ──WebSocket──► ESP32-S2 HTTP server (main/main.c)
                              │
                              ├─ Text commands (show/get/set/save/reset)
                              │   passed as ASCII to ESC firmware
                              │
                              └─ Binary protocol (probe/info/update)
                                  val+~val byte pairs with CRC32
                                      │
                              RS-485 UART ──► Remora™ eCom board
                                             (ESCape32 firmware)
```

### eCom Tab — Motor Presets

The eCom tab exposes the 12 parameters most relevant to slot car operation:

| Parameter | Purpose |
|---|---|
| `damp` | Complementary PWM — enable unless limiting output voltage |
| `revdir` | Reverse motor direction |
| `timing` | Advance timing — higher = faster no-load speed, more current |
| `freq_min` / `freq_max` | PWM frequency — low = lower MOSFET losses; high-Kv motors benefit from higher frequency |
| `duty_min` / `duty_max` | Output voltage range — normally 100% |
| `duty_spup` | Spin-up power limit — caps inrush at startup |
| `duty_ramp` | Power at kERPM ramp threshold |
| `duty_rate` | Duty cycle slew rate — lower = softer start |
| `analog_min` / `analog_max` | Analog input setpoints — normally 0 / 1440 |

Factory presets built in for 9 motor specs (2,000 Kv – 22,000 Kv). You can also:
- **Save to Device** — stores values in ESP32 NVS flash under a custom name
- **⬇ Export JSON** — downloads current values as a `.json` file
- Load a saved preset back by clicking its tag in the saved-presets row

### NVS Storage Layout

| Namespace | Key | Value |
|---|---|---|
| `aart_wifi` | `ssid` | AP SSID (default: `ESCape32-WiFi-Link`) |
| `aart_wifi` | `pass` | AP password (default: empty = open) |
| `aart_pre` | `<slug>` | JSON blob of a saved parameter set |

---

## Requirements

### ESP32-S2 build
- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/) 5.x
- ESP32-S2 with RS-485 UART connected to Remora™ board

### Desktop development
- Python 3.7+ (mock server + tests — stdlib only, no pip installs)
- gcc or clang, make, pthreads (native C build)

---

## Project

**AART — Adrian & Richard's Technologies**  
[aart.dev](https://aart.dev) · [GitHub](https://github.com/adrianblakey/slot-car-ecom)  
ESCape32 firmware: [escape32.org](https://escape32.org)

© Adrian & Richard's Technologies. Hardware designs open source.  
Firmware uses [ESCape32](https://github.com/neoxic/ESCape32) — GPL-3.0.
