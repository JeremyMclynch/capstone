# UWB Mesh Tracker

Real-time indoor positioning using Ultra-Wideband (DS-TWR) ranging over a Thread mesh network.

```
┌──────────────────────────────────────────────────────────────────┐
│                        Thread Mesh Network                       │
│                                                                  │
│  ┌───────────────┐    UWB ranging    ┌───────────────────────┐  │
│  │  DWM3001CDK   │◄────────────────►│    nRF52840 DK        │  │
│  │  (Tag 0x0100) │                   │ + DWM3000EVB Shield   │  │
│  │  mobile node  │                   │  (Anchor 0x0001)      │  │
│  └───────────────┘                   └───────────────────────┘  │
│                                                                  │
│  ┌───────────────┐                                              │
│  │   XIAO BLE    │    CoAP POST /distance + /event             │
│  │  (Tag 0x0200) │    over Thread multicast ff03::1            │
│  └───────────────┘                                              │
└──────────────────────────────────────────────────────────────────┘
                               │
                      nRF USB Dongle
                      (OpenThread RCP)
                          wpan0
                               │
                   ┌───────────▼───────────┐
                   │     Linux Host        │
                   │  ot-daemon (Leader)   │
                   │  monitor.py / server  │
                   └───────────────────────┘
```

nRF boards perform UWB ranging; distance measurements are sent via CoAP over Thread to a Linux host running a CoAP server with SQLite storage.

## Prerequisites

| Tool | Purpose | Install |
|---|---|---|
| [nrfutil](https://www.nordicsemi.com/Products/Development-tools/nRF-Util) | Toolchain + flash | `nrfutil toolchain-manager install --toolchain-bundle-id v3.2.2` |
| [west](https://docs.zephyrproject.org/latest/develop/west/) | Zephyr meta-tool | `pip install west` |
| Python 3.10+ | Host tools | System package manager |
| Go 1.21+ | mcumgr CLI (OTA) | System package manager |

## Setup (Fresh Clone)

```bash
# 1. Clone the repository into a workspace directory
mkdir uwb-workspace && cd uwb-workspace
git clone <repo-url> capstone

# 2. Initialize west workspace and fetch SDK dependencies (~5 GB, takes 10-30 min)
west init -l capstone
west update

# 3. Install nRF toolchain (if not already installed, ~3 GB)
nrfutil toolchain-manager install --toolchain-bundle-id v3.2.2

# 4. Build host tools (OpenThread ot-daemon/ot-ctl, mcumgr CLI)
cd capstone
bash tools/setup_host_tools.sh

# 5. Python dependencies (for monitor and server)
pip install aiocoap pyserial
```

This creates a [T2 west workspace](https://docs.zephyrproject.org/latest/develop/west/workspaces.html) where the SDK repos (`zephyr/`, `nrf/`, `bootloader/`, etc.) are siblings of `capstone/`. All SDK directories are gitignored.

## Build & Flash

```bash
# Build only
bash firmware/build.sh nrf52840dk/nrf52840           # anchor
bash firmware/build.sh decawave_dwm3001cdk           # tag
bash firmware/build.sh xiao_ble                      # XIAO BLE tag

# Build + flash (single board connected)
bash firmware/build.sh nrf52840dk/nrf52840 --flash
bash firmware/build.sh decawave_dwm3001cdk --flash

# Clean rebuild
bash firmware/build.sh nrf52840dk/nrf52840 --clean --flash
```

When both J-Link boards are connected, flash manually with `--dev-id`:
```bash
west flash --build-dir firmware/build/nrf52840dk-nrf52840 --dev-id 1050222631   # anchor
west flash --build-dir firmware/build/decawave_dwm3001cdk --dev-id 760206311    # tag
```

XIAO BLE uses UF2 drag-and-drop: double-tap RST, drag `firmware/build/xiao_ble/zephyr/zephyr.uf2` to the USB drive.

## Running the System (After Reboot)

Everything below assumes the firmware is already flashed and Python dependencies are installed. This is what you need to do each time you reboot or log back in.

### 1. Verify hardware is connected

Plug in the USB dongle and both dev boards, then confirm they're enumerated:

```bash
nrfutil device list
```

You should see 3 devices: the nRF USB Dongle (OpenThread RCP), the nRF52840 DK (anchor), and the DWM3001CDK (tag). Note the serial port assignments — **they can change between reboots**. Check the `Ports` field for each device and compare with the table in the Hardware section below.

### 2. Start the Thread network on the host

```bash
sudo bash tools/scripts/thread_dongle_setup.sh
```

This starts `ot-daemon` on the USB dongle, configures the Thread dataset, and joins the mesh as Leader. The `ot-daemon` process does **not** persist across reboots — you must re-run this script each time.

The script will print the `wpan0` IPv6 addresses and Thread state when done.

### 3. Wait for devices to join the mesh

The firmware on both boards auto-starts ranging on boot — no manual action needed on the boards themselves. After the host Thread network is up, the boards join within ~10-15 seconds.

Verify they've joined by pinging the Thread multicast address:

```bash
ping6 -c3 -I wpan0 ff03::1
```

You should see responses from 2 devices (anchor + tag). If no responses appear after 15 seconds, check that:
- Both boards are powered (LEDs active)
- The USB dongle port in `thread_dongle_setup.sh` matches `nrfutil device list` output
- Try resetting the boards: `nrfutil device reset --serial-number <S/N>`

### 4. Run the monitor

```bash
python3 tools/monitor.py
```

You should see `[DIST ...]` lines arriving within a few seconds — one per ranging cycle. Each line shows the anchor/tag IDs, measured distance, and update rate.

For persistent storage instead of console output:

```bash
cd tools/server && python3 main.py
```

### 5. (Alternative) All-in-one dashboard

Instead of running `monitor.py` and `uwb_tool.py` in separate terminals, use the curses-based dashboard that combines monitoring, device discovery, and UCI commands in one screen:

```bash
python3 tools/dashboard.py
```

Use arrow keys to navigate commands, Enter to execute, and `q` to quit. Devices are auto-discovered via Thread multicast.

### 6. (Optional) Verify devices via UCI over CoAP

Use the IPv6 addresses from the `ping6` output to query devices remotely:

```bash
python3 tools/scripts/uwb_tool.py coap://[<device-ipv6>] info
python3 tools/scripts/uwb_tool.py coap://[<device-ipv6>] status
```

## Hardware

| Device | Role | J-Link S/N | UART |
|---|---|---|---|
| nRF USB Dongle | OpenThread RCP | — | /dev/ttyACM0 |
| nRF52840 DK + DWM3000EVB | Anchor (0x0001) | 1050222631 | /dev/ttyACM1 |
| DWM3001CDK | Tag (0x0100) | 760206311 | /dev/ttyACM3 |
| XIAO BLE + DWM3000EVB | Tag (0x0200) | — | UF2 flash |

Use `nrfutil device list` to identify serial port assignments (they can change between reboots).

## Thread Network Credentials

All devices share these compiled-in credentials:

| Parameter | Value |
|---|---|
| Channel | 15 |
| PAN ID | 0xABCD (43981) |
| Network Key | `00:11:22:33:44:55:66:77:88:99:aa:bb:cc:dd:ee:ff` |
| Extended PAN ID | `11:11:11:11:22:22:22:22` |
| Network Name | UWBTracker |

## UCI Device Configuration

Devices use a binary UCI protocol over UART (not a text shell) for runtime configuration:

```bash
python3 tools/scripts/uwb_tool.py /dev/ttyACM1 info          # device info
python3 tools/scripts/uwb_tool.py /dev/ttyACM1 status        # ranging status
python3 tools/scripts/uwb_tool.py /dev/ttyACM1 start         # start ranging
python3 tools/scripts/uwb_tool.py /dev/ttyACM1 stop          # stop ranging
python3 tools/scripts/uwb_tool.py /dev/ttyACM3 set-interval 500  # change interval (ms)
python3 tools/scripts/uwb_tool.py /dev/ttyACM1 save          # persist config to NVS
python3 tools/scripts/uwb_tool.py /dev/ttyACM1 factory-reset # reset all settings
```

Configuration is persisted to NVS flash via `save`. See `firmware/ARCHITECTURE.md` for the full UCI command reference.

## OTA Firmware Updates

Firmware updates use MCUmgr SMP over UDP (anchor) or serial recovery (tag):

```bash
# Anchor: OTA over Thread IPv6
./tools/scripts/ota_update.sh <device-ipv6-addr> firmware/build/nrf52840dk-nrf52840/firmware/zephyr/zephyr.signed.bin

# Tag: serial recovery (hold P0.02 button during reset, then upload)
mcumgr --conntype serial --connstring dev=/dev/ttyACM3,baud=115200 \
    image upload firmware/build/decawave_dwm3001cdk/firmware/zephyr/zephyr.signed.bin
```

Bump `firmware/VERSION` before building update images.

## Project Structure

```
uwb-workspace/                  # West workspace root
├── .west/                      # West metadata (created by west init)
├── capstone/                   # ← this repository (manifest repo)
│   ├── west.yml                # West manifest (nRF SDK v3.2.2 + DW3000 driver)
│   ├── firmware/               # Zephyr RTOS application (shared for all boards)
│   │   ├── src/
│   │   │   ├── main.c          # Boot sequence + module init
│   │   │   ├── uwb_manager.c/h    # DS-TWR ranging loop (interrupt-driven)
│   │   │   ├── thread_coap.c/h    # Thread + CoAP distance/event reporting
│   │   │   ├── device_config.c/h  # NVS-backed persistent configuration
│   │   │   ├── uci.c/h            # UCI binary protocol parser + dispatch
│   │   │   ├── uci_uart.c         # UCI UART transport (ISR RX → state machine)
│   │   │   ├── uci_coap.c         # UCI CoAP transport (remote config)
│   │   │   └── leds.h             # LED abstraction
│   │   ├── boards/             # Per-board Kconfig + devicetree overlays
│   │   ├── prj.conf            # Base Kconfig
│   │   ├── CMakeLists.txt
│   │   ├── build.sh            # Build script (auto-detects west workspace)
│   │   ├── build_multi.sh      # Build all targets
│   │   ├── VERSION             # Firmware version (for MCUboot signing)
│   │   ├── ARCHITECTURE.md     # Detailed firmware architecture
│   │   └── API_REFERENCE.md    # Non-standard API call catalog
│   │
│   └── tools/
│       ├── monitor.py          # Live CoAP distance/event monitor
│       ├── dashboard.py        # Curses TUI (monitor + discovery + commands)
│       ├── setup_host_tools.sh # Build OpenThread + install mcumgr
│       ├── scripts/
│       │   ├── uwb_tool.py         # UCI command-line tool
│       │   ├── thread_dongle_setup.sh  # Thread network setup (run as root)
│       │   └── ota_update.sh       # OTA firmware update helper
│       └── server/
│           ├── main.py         # CoAP server entry point
│           ├── coap_server.py  # /distance resource handler
│           ├── coap_receiver.py    # Standalone debug receiver
│           ├── database.py     # SQLite storage layer
│           └── requirements.txt
│
├── zephyr/                     # ← fetched by west update
├── nrf/                        # ← fetched by west update
├── bootloader/mcuboot/         # ← fetched by west update
├── modules/                    # ← fetched by west update
└── zephyr-dw3000-decadriver/   # ← fetched by west update (DW3000 Zephyr driver)
```

## Data Flow

```
Tag ──UWB pulse──► Anchor
                    │
                    ▼ (distance_mm computed via DS-TWR)
              thread_coap_send_distance()
                    │
                    ▼ CoAP POST ff03::1:5683/distance
              [anchor_id: u16][tag_id: u16]
              [distance_mm: u32][uptime_s: u32]
                    │  (12 bytes, little-endian)
                    ▼ Thread mesh → RCP dongle → wpan0
              Linux host (ot-daemon)
                    │
                    ▼
           monitor.py     (live console)
                 OR
           server/main.py → SQLite (production)
```

## Further Documentation

- **[firmware/ARCHITECTURE.md](firmware/ARCHITECTURE.md)** — boot sequence, thread/interrupt model, DS-TWR protocol, state machines, CoAP payloads, UCI commands, config system, OTA details
- **[firmware/API_REFERENCE.md](firmware/API_REFERENCE.md)** — catalog of all non-standard API calls with SDK on-disk locations (DW3000 driver, OpenThread, nRF SDK, Zephyr subsystems)
