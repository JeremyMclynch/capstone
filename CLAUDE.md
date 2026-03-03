# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

UWB Mesh Tracker — real-time indoor positioning using Ultra-Wideband (DS-TWR) ranging over a Thread mesh network. nRF boards perform UWB ranging; distance measurements are sent via CoAP over Thread to a Linux host.

## Setup (First-Time)

```bash
# 1. Initialize west workspace and fetch all SDK dependencies (~5 GB)
west init -l .
west update

# 2. Install nRF toolchain (if not already installed)
nrfutil toolchain-manager install --toolchain-bundle-id v3.2.2

# 3. Build host tools (OpenThread ot-daemon/ot-ctl, mcumgr CLI)
bash tools/setup_host_tools.sh

# 4. Python dependencies (for monitor and server)
pip install aiocoap pyserial
```

The `west.yml` manifest declares all SDK dependencies (nRF Connect SDK v3.2.2, Zephyr, MCUboot, DW3000 driver). The build script auto-detects the west workspace and falls back to `~/ncs/v3.2.2` if `.west/` doesn't exist.

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

# Multi-node build (all anchors + tags with different addresses)
bash firmware/build_multi.sh
```

**When both boards are connected**, `west flash` refuses to guess which to target. Flash manually:
```bash
west flash --build-dir ~/Projects/capstone/firmware/build/nrf52840dk-nrf52840 --dev-id 1050222631   # anchor
west flash --build-dir ~/Projects/capstone/firmware/build/decawave_dwm3001cdk --dev-id 760206311    # tag
```

Board targets must include the SoC qualifier: `nrf52840dk/nrf52840` not `nrf52840dk`.

## Hardware

Use `nrfutil device list` to identify serial port assignments (they can change between reboots).

| Device | Role | Serial/dev-id | UART |
|---|---|---|---|
| nRF52840 DK + DWM3000EVB | Anchor (DS-TWR responder) | 1050222631 | /dev/ttyACM1 |
| DWM3001CDK | Tag (DS-TWR initiator) | 760206311 | /dev/ttyACM3 |
| nRF52840 USB Dongle | Thread RCP | — | /dev/ttyACM0 |

### XIAO BLE

Board target: `xiao_ble`. App-only build (no MCUboot, uses `--no-sysbuild`). Default address 0x0200. Flash via UF2 drag-and-drop: double-tap RST, drag `zephyr.uf2` to the USB drive. DK library works for LEDs (3 RGB LEDs, no buttons). Board conf disables MCUmgr/IMG_MANAGER/RETENTION_BOOT_MODE.

## UCI Device Configuration

Binary protocol over UART (`tools/scripts/uwb_tool.py`), not a text shell:
```bash
python3 tools/scripts/uwb_tool.py /dev/ttyACM1 info
python3 tools/scripts/uwb_tool.py /dev/ttyACM1 status
python3 tools/scripts/uwb_tool.py /dev/ttyACM1 stop
python3 tools/scripts/uwb_tool.py /dev/ttyACM1 start
python3 tools/scripts/uwb_tool.py /dev/ttyACM3 set-interval 500
python3 tools/scripts/uwb_tool.py /dev/ttyACM1 save          # persist to NVS flash
python3 tools/scripts/uwb_tool.py /dev/ttyACM1 factory-reset
```

Frame format: `[0xAA][CMD][LEN][PAYLOAD][CRC8]` → `[0xBB][CMD][STATUS][LEN][PAYLOAD][CRC8]`

## Monitoring

```bash
# Start Thread on Linux host (once, as root)
sudo bash tools/scripts/thread_dongle_setup.sh

# Live CoAP monitor (distance + tag events)
python3 tools/monitor.py

# Full CoAP server with SQLite storage
cd tools/server && python3 main.py
```

## OTA Firmware Updates

MCUmgr SMP over UDP + MCUboot bootloader. Signed images uploaded over Thread IPv6:
```bash
# Upload new firmware
./tools/scripts/ota_update.sh <device-ipv6-addr> firmware/build/<board>/firmware/zephyr/zephyr.signed.bin

# Bump version before building update (firmware/VERSION)
```

Anchor (nRF52840): dual-slot with swap + rollback over Thread. Tag (DWM3001CDK): single-slot MCUboot with serial recovery — hold button (P0.02) during reset, then upload via `mcumgr --conntype serial`. Flash both boards using `merged.hex` (not `zephyr.hex`) to include MCUboot + signed app.

Tag serial recovery:
```bash
# Hold P0.02 button, reset board, then:
mcumgr --conntype serial --connstring dev=/dev/ttyACM3,baud=115200 image upload firmware/build/decawave_dwm3001cdk/firmware/zephyr/zephyr.signed.bin
```

## Architecture

Shared firmware source builds for all boards; board-specific config in `firmware/boards/*.conf` and `*.overlay`. Role (anchor/tag) is set via Kconfig defaults per board but overridable at runtime via UCI + NVS.

**Firmware modules** (all in `firmware/src/`):
- `main.c` — boot sequence: device_config → thread_coap → uci_coap → uwb_manager → uci_uart → autostart
- `uwb_manager.c/h` — DS-TWR ranging loop (interrupt-driven, priority 0), start/stop/status API
- `thread_coap.c/h` — Thread join + CoAP POST `/distance` (anchor) and `/event` (tag) to multicast
- `device_config.c/h` — NVS-backed persistent config (role, addr, interval, server, autostart)
- `uci.c/h` — UCI binary protocol parser and command dispatch (mutex-protected)
- `uci_uart.c` — UART transport (ISR RX ring buffer → state machine → response TX)
- `uci_coap.c` — CoAP transport (OT CoAP server, POST /cmd, remote UCI)

**Config layering**: Kconfig defaults → board `.conf` overrides → NVS saved values (loaded by device_config_init at boot)

## Firmware Documentation

- `firmware/ARCHITECTURE.md` — detailed firmware architecture, boot sequence, thread/interrupt model, DS-TWR protocol, state machines, CoAP payloads, UCI commands, config system, OTA, and IPv6/CoAP server integration guide
- `firmware/API_REFERENCE.md` — catalog of all non-standard API calls with SDK on-disk locations (DW3000 driver, OpenThread, nRF SDK, Zephyr subsystems)

## Key Gotchas

- `CONFIG_UART_INTERRUPT_DRIVEN=y` is required in prj.conf — shell used to pull it in, but shell is now disabled to save RAM
- `CONFIG_OPENTHREAD_PANID` is type `int` — use decimal `43981` not hex `0xABCD`
- DW3000 driver API differences from Qorvo examples: `dwt_readrxtimestamp(buf, DWT_IP_M)`, `dwt_getframelength(NULL)`, don't redefine `DWT_TIME_UNITS`/`FCS_LEN`
- DW3000 Zephyr driver managed by west (`west.yml`), cloned to `./zephyr-dw3000-decadriver/` (gitignored)
- Tag (DWM3001CDK) uses DW3720 chip variant; anchor (nRF52840 DK + DWM3000EVB) uses DW3000
- UWB thread uses `k_sched_lock()` around timing-critical TX/RX sequences; preamble timeout is 0 for anchor's FINAL wait
- Stop/start ranging requires draining semaphores and clearing DW3000 status bits to avoid stale state
- Use pyserial scripts to read UART output (screen/cat unreliable after flash)
- `references/` contains vendor SDK docs (Qorvo DW3000 SDK, nRF SDK docs) — gitignored, not needed for build

## Thread Network

Channel 15, PAN ID 0xABCD (43981), network key `00:11:22:33:44:55:66:77:88:99:aa:bb:cc:dd:ee:ff`. All devices and `tools/scripts/thread_dongle_setup.sh` must match.
