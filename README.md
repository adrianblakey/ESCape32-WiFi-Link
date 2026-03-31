# AART Remora™ Programmer

Web-based wireless programmer for the [Remora™ electronic commutator (eCom)](https://aart.dev) — a brushless motor speed controller for slot car racing built around the ESCape32 firmware. Runs on an ESP32-S2 Wi-Fi module; any browser on any device connects without installing an app.

---

## File Structure

```
aart_remora_programmer/
│
├── README.md                     ← This file
├── CMakeLists.txt                ← ESP-IDF top-level project file
├── sdkconfig.defaults            ← ESP32-S2 pin defaults (UART, LED)
├── mock_server.py                ← Python desktop simulator (no ESP32 needed)
├── test_mock.py                  ← Automated Python test suite
│
└── main/                         ← ESP-IDF component (application source)
    ├── CMakeLists.txt            ← Embeds & gzips root*.* at build time
    ├── main.c                    ← HTTP server, WebSocket bridge, DNS, NVS
    ├── build_defs.h.in           ← CMake template → build_defs.h
    ├── Kconfig.projbuild         ← menuconfig: UART pins, LED pin
    ├── idf_component.yml         ← Component dependencies (mdns)
    ├── root.html                 ← Single-page web UI (6 tabs, 6 languages)
    ├── root_en.json              ← UI strings – English
    ├── root_de.json              ← UI strings – German
    ├── root_fr.json              ← UI strings – French
    ├── root_it.json              ← UI strings – Italian
    ├── root_ru.json              ← UI strings – Russian
    └── root_zh.json              ← UI strings – Chinese (Simplified)
```

---

## Quick Start

### Desktop simulation — Python mock server (no ESP32 needed)

The mock server perfectly simulates the ESP32's HTTP and WebSocket behaviour,
including realistic eRPM / voltage telemetry driven by the throttle slider.

```bash
# From the project root:
python3 mock_server.py

# Open in your browser:
open http://localhost:8080        # macOS
xdg-open http://localhost:8080    # Linux

# Custom port:
python3 mock_server.py --port 9090

# Run the automated test suite:
python3 test_mock.py --start-server
```

### Flash to ESP32-S2

```bash
# Install ESP-IDF 5.x, then from the project root:
idf.py set-target esp32s2
idf.py build flash monitor

# sdkconfig.defaults sets the correct pins automatically.
# To change pins:  idf.py menuconfig → ESCape32-WiFi-Link configuration

# On the device:
#   Connect to Wi-Fi AP:  ESCape32-WiFi-Link  (open, no password by default)
#   Open browser:         http://192.168.4.1
#              or:        http://escape32.local
```

---

## How It Works

```
Browser ──WebSocket──► ESP32-S2 HTTP/WS server (main/main.c)
                               │
                               ├─ Text commands (show / get / set / save / reset /
                               │   info / throt / play / _wifi_get / _wifi_set /
                               │   _preset_save)
                               │   passed as ASCII over RS-485 UART to ESC
                               │
                               └─ Binary protocol (_probe / _info / _update)
                                   val+~val byte pairs with CRC32
                                       │
                               RS-485 UART ──► Remora™ eCom board
                                              (ESCape32 firmware)
```

All static content (HTML, JSON language files) is gzipped and embedded in
the firmware at build time by the CMake build system — no file system or SD
card is required on the ESP32-S2.

---

## UI Tabs

### eCom — Essential motor parameters

The 12 parameters most relevant to slot car operation, with a motor preset
system backed by the Motor Database.

| Parameter | Purpose |
|---|---|
| `damp` | Complementary PWM — enable unless limiting output voltage |
| `revdir` | Reverse motor direction |
| `timing` | Advance timing — higher = faster no-load speed, more current |
| `freq_min` / `freq_max` | PWM frequency kHz — low = lower losses; high-Kv motors may need 48+ kHz |
| `duty_min` / `duty_max` | Output voltage range — normally 100% |
| `duty_spup` | Spin-up power limit — caps inrush current at startup |
| `duty_ramp` | Power ceiling at the kERPM ramp threshold |
| `duty_rate` | Duty cycle slew rate — lower = softer throttle response |
| `analog_min` / `analog_max` | Analog input setpoints — normally 0 / 1440 |

The live telemetry bar at the bottom shows eRPM, voltage, and current. When a
motor with a known pole count is selected, mechanical RPM and Kv are also
displayed and updated in real time as the throttle slider moves.

### Motors — Motor database

A browser-local IndexedDB database of motor specifications. Fields include
vendor, model, Kv (rated and measured), pole count, stator geometry,
winding details, rotor dimensions, shaft, fixation, inductance, resistance,
mass, mounting dimensions, and a reference URL. Nine built-in motors are
pre-loaded (EMAX, Parma, Do-Slot, AMAX, AART). Any motor in the database
can be selected as the active motor for RPM / Kv display in the eCom tab.

### Firmware — OTA update

Upload a new ESCape32 binary directly from the browser to the connected ESC
over the Wi-Fi link. Supports bootloader and firmware image targets,
write-protection control, and a real-time progress bar.

### Wi-Fi — Access Point configuration

Change the AP SSID and password. Settings are saved to ESP32 NVS flash and
take effect after the next power cycle.

### Settings — Full parameter table

Complete ESCape32 parameter set with live throttle control and the same
eRPM / RPM / Kv telemetry display as the eCom tab.

### Music — RTTTL melody editor

Edit and play the ESC startup melody using RTTTL notation, with volume
and beacon controls.

---

## Language Support

The UI is fully internationalised. Switch language with the selector in the
top-right corner. All strings — including eCom parameter hints, motor
database field labels, and tab content — switch instantly without a page
reload. Supported languages: **English, German, French, Italian, Russian,
Chinese (Simplified)**.

Language strings are embedded inline in the page at build time; no network
request is needed to switch language, so it works even when the WebSocket
connection is open.

---

## NVS Storage Layout

| Namespace | Key | Value |
|---|---|---|
| `remora` | `ssid` | AP SSID (default: `ESCape32-WiFi-Link`) |
| `remora` | `pass` | AP password (default: empty — open network) |
| `remora` | `p_<slug>` | JSON blob of a saved ESC parameter preset |

---

## Hardware

| Signal | ESP32-S2 GPIO |
|---|---|
| UART TX (to ESC) | 33 |
| UART RX (from ESC) | 16 |
| Status LED | 15 |

Pins can be changed via `idf.py menuconfig` → **ESCape32-WiFi-Link
configuration**, or by editing `sdkconfig.defaults` before the first build.

The UART runs at 38,400 baud, RS-485 half-duplex mode.

---

## Requirements

### ESP32-S2 build
- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/) 5.x
- ESP32-S2 module
- Remora™ eCom board (ESCape32 firmware) connected via RS-485 UART

### Desktop simulation
- Python 3.7+ (mock server and tests — stdlib only, no pip installs required)

---

## Project

**AART — Adrian & Richard's Technologies**  
[aart.dev](https://aart.dev)  
ESCape32 firmware: [escape32.org](https://escape32.org)

© Adrian & Richard's Technologies. Hardware designs open source.  
Firmware uses [ESCape32](https://github.com/neoxic/ESCape32) — GPL-3.0.
