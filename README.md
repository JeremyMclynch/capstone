# UWB Mesh Tracker — System Overview

A real-time indoor positioning system using Ultra-Wideband (UWB) ranging over a Thread mesh network.

```
┌─────────────────────────────────────────────────────────────────┐
│                        Thread Mesh Network                       │
│                                                                  │
│  ┌───────────────┐    UWB ranging    ┌───────────────────────┐  │
│  │  DWM3001CDK   │◄─────────────────►│    nRF52840 DK        │  │
│  │  (Tag 0x0100) │                   │ + DWM3000EVB Shield   │  │
│  │  mobile node  │  CoAP POST over   │  (Anchor 0x0001)      │  │
│  └───────────────┘  Thread multicast └───────────────────────┘  │
│                      ff03::1:5683/distance                       │
└─────────────────────────────────────────────────────────────────┘
                                │
                       nRF USB Dongle
                       (OpenThread RCP)
                           wpan0
                                │
                    ┌───────────▼───────────┐
                    │    Linux Host         │
                    │  ot-daemon (Leader)   │
                    │  coap_receiver.py     │
                    │  or main.py + SQLite  │
                    └───────────────────────┘
```

---

## Hardware

| Device | Role | J-Link S/N | Serial Port |
|---|---|---|---|
| nRF USB Dongle | OpenThread RCP (radio) | — | /dev/ttyACM0 |
| nRF52840 DK + DWM3000EVB | Anchor (0x0001) | 1050222631 | /dev/ttyACM1 |
| DWM3001CDK | Tag (0x0100) | 760206311 | /dev/ttyACM3 |

---

## Thread Network Credentials

All devices share these compiled-in credentials:

| Parameter | Value |
|---|---|
| Channel | 15 |
| PAN ID | 0xABCD (43981) |
| Network Key | `00:11:22:33:44:55:66:77:88:99:aa:bb:cc:dd:ee:ff` |
| Extended PAN ID | `11:11:11:11:22:22:22:22` |
| Network Name | UWBTracker |

---

## Project Structure

```
capstone/
├── firmware/               # Zephyr RTOS application (shared for all boards)
│   ├── src/
│   │   ├── main.c          # Entry point: init UWB + Thread, main loop
│   │   ├── uwb_manager.c   # DW3000/DW3720 UWB ranging logic
│   │   ├── uwb_manager.h
│   │   ├── thread_coap.c   # Thread network + CoAP distance reporting
│   │   └── thread_coap.h
│   ├── boards/
│   │   ├── nrf52840dk_nrf52840.conf     # Anchor: DW3000 chip, role=ANCHOR, addr=0x0001
│   │   ├── nrf52840dk_nrf52840.overlay  # SPI/GPIO pin assignments for DWM3000EVB shield
│   │   ├── decawave_dwm3001cdk.conf     # Tag: DW3720 chip, role=TAG, addr=0x0100
│   │   └── decawave_dwm3001cdk.overlay  # Pin assignments for DWM3001CDK onboard UWB
│   ├── prj.conf            # Base Kconfig: OpenThread FTD, CoAP, DW3000, logging
│   ├── CMakeLists.txt
│   ├── Kconfig
│   ├── build.sh            # Build (and optionally flash) a specific board target
│   └── build_multi.sh      # Build all targets in one go
│
└── server/                 # Linux host — receives distance data from Thread mesh
    ├── thread_setup.sh     # One-time Thread network configuration (run as root)
    ├── coap_receiver.py    # Standalone CoAP debug receiver (console logging only)
    ├── main.py             # Production CoAP server entry point
    ├── coap_server.py      # CoAP /distance resource handler
    ├── database.py         # SQLite storage layer
    └── requirements.txt    # Python dependencies (aiocoap)
```

---

## File Descriptions

### Firmware

#### `firmware/prj.conf`
The base Zephyr Kconfig for both board targets. Enables:
- `CONFIG_OPENTHREAD_FTD` — Full Thread Device (can act as Router/Leader)
- `CONFIG_COAP` / `CONFIG_COAP_UTILS` — CoAP messaging
- `CONFIG_DW3000` — UWB radio driver
- Thread network credentials (channel, PAN ID, network key)
- Shell + OpenThread shell for UART debugging

#### `firmware/boards/nrf52840dk_nrf52840.conf`
Anchor-specific settings: selects the DW3000 chip variant, sets `NODE_ROLE_ANCHOR`, and assigns short address `0x0001`.

#### `firmware/boards/nrf52840dk_nrf52840.overlay`
Devicetree overlay for the DWM3000EVB Arduino shield on the nRF52840 DK:
- Disables UART1 (frees pins used by the shield for SPI control)
- Assigns DW3000 SPI chip-select (P1.12), IRQ (P1.10), RESET (P1.08), WAKEUP (P1.11)

#### `firmware/boards/decawave_dwm3001cdk.conf`
Tag-specific settings: selects the DW3720 chip variant, sets `NODE_ROLE_TAG`, and assigns short address `0x0100`.

#### `firmware/src/thread_coap.c`
Manages the OpenThread network connection and CoAP reporting:
- Registers a Thread role-change callback — sets `thread_connected` when the device joins the mesh as Child, Router, or Leader
- Queues distance measurements (up to 8 deep) and dispatches them on a dedicated work queue
- Sends `POST /distance` to the multicast address `ff03::1` (all Thread nodes, realm-local scope) on port 5683
- Payload: 12-byte little-endian struct — `anchor_id (u16)`, `tag_id (u16)`, `distance_mm (u32)`, `uptime_s (u32)`
- Drops oldest measurement if queue is full (non-blocking)

#### `firmware/build.sh`
Convenience build script. Sets up the nRF Connect SDK toolchain environment variables and calls `west build` / `west flash`.

```bash
bash firmware/build.sh nrf52840dk/nrf52840 --flash          # build + flash anchor
bash firmware/build.sh decawave_dwm3001cdk --flash          # build + flash tag
bash firmware/build.sh nrf52840dk/nrf52840 --clean --flash  # clean rebuild
```

---

### Server

#### `server/thread_setup.sh`
**Run once as root** after plugging in the nRF USB dongle. Configures `ot-daemon` with the matching Thread credentials and starts the Linux host as Thread **Leader** (the network coordinator).

```bash
sudo bash server/thread_setup.sh
```

What it does:
1. Stops any existing Thread session
2. Programs channel, network key, PAN ID, extended PAN ID, and network name into the active dataset
3. Brings up the `wpan0` TUN interface and starts the Thread stack
4. Prints the assigned IPv6 addresses for confirmation

The firmware devices (anchor + tag) will auto-attach to this network using their compiled-in matching credentials.

#### `server/coap_receiver.py`
A lightweight standalone CoAP server for **testing and debugging**. It:
- Joins the `ff03::1` IPv6 multicast group on `wpan0` so it receives the anchor's multicast CoAP packets
- Listens on `[::]:5683` for `POST /distance`
- Decodes the 12-byte payload and logs each measurement to the console

Use this to quickly verify the end-to-end data flow without a database:

```bash
python3 server/coap_receiver.py
```

Expected output when working:
```
21:05:55  INFO   Joined multicast ff03::1 on wpan0 (ifindex=8)
21:05:55  INFO   CoAP server listening on [::]:5683 — waiting for distance data...
21:08:12  INFO   [#1] anchor=0x0001  tag=0x0100  dist=1.234 m  uptime=17s  from=...
```

#### `server/main.py`
The **production CoAP server**. Starts the same CoAP listener but stores every measurement in a SQLite database. Supports command-line arguments:

```bash
python3 server/main.py                          # defaults: :: port 5683, uwb_measurements.db
python3 server/main.py --host :: --port 5683 --db /tmp/uwb.db
```

#### `server/coap_server.py`
CoAP resource handler used by `main.py`. Implements the `/distance` resource:
- Validates the 12-byte payload size
- Decodes `anchor_id`, `tag_id`, `distance_mm`, `uptime_s`
- Warns if distance is outside plausible UWB range (0.1 – 200 m)
- Stores to SQLite via `database.insert_measurement()`

#### `server/database.py`
SQLite storage layer. Creates the `measurements` table on first run with:
- `anchor_id`, `tag_id`, `distance_mm`, `node_uptime_s`, `received_at` (auto-timestamped)
- A virtual `distance_m` column (millimetres ÷ 1000)
- Indexes on `(anchor_id, tag_id)` and `received_at`
- A `latest_distances` view for quick lookup of the most recent reading per anchor-tag pair

---

## Quick Start

### 1. Flash firmware
```bash
# Anchor (nRF52840 DK + DWM3000EVB shield)
bash firmware/build.sh nrf52840dk/nrf52840 --flash

# Tag (DWM3001CDK) — specify J-Link serial to target the right board
cd ~/ncs/v3.2.2
west flash --build-dir ~/Projects/capstone/firmware/build/decawave_dwm3001cdk --snr 760206311
```

### 2. Start Thread network on Linux host
```bash
sudo bash server/thread_setup.sh
```

### 3. Run the CoAP receiver
For quick testing (console output only):
```bash
python3 server/coap_receiver.py
```

For production (stores to SQLite):
```bash
python3 server/main.py
```

### 4. Verify
After ~10 seconds the firmware devices join the Thread mesh. The anchor starts ranging with the tag and sends `POST /distance` to `ff03::1:5683` every measurement cycle. The receiver logs each arriving measurement.

---

## Data Flow

```
Tag ──UWB pulse──► Anchor
                    │
                    ▼ (distance_mm computed via ToA)
              thread_coap_send_distance()
                    │
                    ▼ CoAP POST ff03::1:5683/distance
              [anchor_id=0x0001][tag_id=0x0100]
              [distance_mm: uint32][uptime_s: uint32]
                    │  (12 bytes, little-endian)
                    ▼ Thread mesh → RCP dongle → wpan0
              Linux host (ot-daemon)
                    │
                    ▼
           coap_receiver.py  (debug)
                   OR
           main.py → SQLite  (production)
```
