# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

UWB Mesh Tracker — real-time indoor positioning using Ultra-Wideband (DS-TWR) ranging over a Thread mesh network. nRF boards perform UWB ranging; distance measurements are sent via CoAP over Thread to a Linux host.

## Setup (First-Time)

```bash
# 1. Initialize west workspace (T2 topology: .west/ in parent dir)
cd ..  # move to parent of project root
west init -l capstone
west update
cd capstone

# 2. Install nRF toolchain (if not already installed)
nrfutil toolchain-manager install --toolchain-bundle-id v3.2.2

# 3. Install Docker (for OTBR) + mcumgr CLI
bash tools/setup_host_tools.sh

# 4. Enable IP forwarding for Thread border router (run once)
curl -sSL https://raw.githubusercontent.com/openthread/ot-br-posix/refs/heads/main/etc/docker/border-router/setup-host | sh

# 5. Python dependencies (for monitor and server)
pip install aiocoap pyserial
```

The `west.yml` manifest declares all SDK dependencies (nRF Connect SDK v3.2.2, Zephyr, MCUboot, DW3000 driver). Standard T2 west workspace: `.west/` and SDK repos live in the parent directory. The build script auto-detects the workspace (checks both project root and parent for `.west/`) and falls back to `~/ncs/v3.2.2` if no workspace exists.

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

**Flash with nrfutil** (works reliably with multiple boards):
```bash
nrfutil device program --firmware ~/Projects/capstone/firmware/build/nrf52840dk-nrf52840/merged.hex --serial-number 1050222631 --options chip_erase_mode=ERASE_ALL
nrfutil device program --firmware ~/Projects/capstone/firmware/build/decawave_dwm3001cdk/merged.hex --serial-number 760206311 --options chip_erase_mode=ERASE_ALL
```

Board targets must include the SoC qualifier: `nrf52840dk/nrf52840` not `nrf52840dk`.

## Hardware

Use `nrfutil device list` to identify serial port assignments (they can change between reboots).

| Device | Role | Serial/dev-id | UART | Thread IPv6 (mesh-local) | UWB Addr |
|---|---|---|---|---|---|
| nRF52840 DK + DWM3000EVB | Anchor | 1050222631 | /dev/ttyACM1 | `fdb6:8266:ea22:f1e0:b34:61f6:812c:46f5` | 0x0AC8 |
| DWM3001CDK | Tag | 760206311 | /dev/ttyACM3 | — | auto |
| XIAO BLE #1 | Anchor | — (J-Link via DK) | — | discovered via `ot-ctl eidcache` | auto |
| XIAO BLE #2 | Anchor | — (J-Link via DK) | — | discovered via `ot-ctl eidcache` | auto |
| nRF52840 USB Dongle | Thread RCP | — | /dev/ttyACM0 | — | — |

**Remote reset via J-Link** (no physical button press needed):
```bash
nrfutil device reset --serial-number 1050222631   # reset anchor (DK)
nrfutil device reset --serial-number 760206311    # reset tag (CDK)
```

### XIAO BLE

Board target: `xiao_ble`. Default address 0x0200. MCUboot dual-slot with OTA over Thread (same as anchor). DK library works for LEDs (3 RGB LEDs, no buttons).

**Flashing**: Uses nRF52840 DK's J-Link Debug Out (SWD) — `nrfutil device` targets the DK's on-board chip, so must use `JLinkExe` instead:
```bash
# Flash via build script (uses JLinkExe internally)
bash firmware/build.sh xiao_ble --flash

# Manual flash via JLinkExe (DK serial 1050222631)
echo -e "loadfile firmware/build/xiao_ble/merged.hex\nr\ng\nexit\n" | \
    JLinkExe -USB 1050222631 -Device nRF52840_xxAA -If SWD -Speed 4000 -autoconnect 1
```

**OTA**: Same as anchor — `./tools/scripts/ota_update.sh <xiao-ipv6> firmware/build/xiao_ble/firmware/zephyr/zephyr.signed.bin`

## UCI Device Configuration

Binary protocol over UART (`tools/scripts/uwb_tool.py`), not a text shell:
```bash
python3 tools/scripts/uwb_tool.py /dev/ttyACM1 info
python3 tools/scripts/uwb_tool.py /dev/ttyACM1 status
python3 tools/scripts/uwb_tool.py /dev/ttyACM1 stop
python3 tools/scripts/uwb_tool.py /dev/ttyACM1 start
python3 tools/scripts/uwb_tool.py /dev/ttyACM3 set-interval 500
python3 tools/scripts/uwb_tool.py /dev/ttyACM1 calibrate      # auto-calibrate from 1m reference
python3 tools/scripts/uwb_tool.py /dev/ttyACM1 get-cal-offset # read current calibration offset
python3 tools/scripts/uwb_tool.py /dev/ttyACM1 set-cal-offset 50  # manually set offset (mm)
python3 tools/scripts/uwb_tool.py /dev/ttyACM1 save          # persist to NVS flash
python3 tools/scripts/uwb_tool.py /dev/ttyACM1 reboot        # reboot device
python3 tools/scripts/uwb_tool.py /dev/ttyACM1 factory-reset
python3 tools/scripts/uwb_tool.py /dev/ttyACM1 cir-enable 50  # capture 50 CIR windows
python3 tools/scripts/uwb_tool.py /dev/ttyACM1 cir-enable     # continuous CIR capture
python3 tools/scripts/uwb_tool.py /dev/ttyACM1 cir-disable    # stop CIR capture
python3 tools/scripts/uwb_tool.py /dev/ttyACM3 peer-list             # list known anchor peers
python3 tools/scripts/uwb_tool.py /dev/ttyACM3 add-peer 0x0001       # manually add anchor peer
python3 tools/scripts/uwb_tool.py /dev/ttyACM3 remove-peer 0x0001    # remove anchor peer
python3 tools/scripts/uwb_tool.py /dev/ttyACM3 set-discovery-interval 10  # discover every 10 cycles
python3 tools/scripts/uwb_tool.py /dev/ttyACM3 discover               # trigger immediate discovery
```

Frame format: `[0xAA][CMD][LEN][PAYLOAD][CRC8]` → `[0xBB][CMD][STATUS][LEN][PAYLOAD][CRC8]`

## Monitoring

```bash
# Start OTBR Docker container and join Thread network
bash tools/scripts/otbr_setup.sh

# OTBR web UI (topology, diagnostics)
# http://localhost:8080

# ot-ctl commands via Docker
docker exec otbr ot-ctl state
docker exec otbr ot-ctl neighbor table
# Or use the wrapper: tools/scripts/ot-ctl state

# Live CoAP monitor (distance + tag events)
python3 tools/monitor.py

# Full CoAP server with SQLite storage
cd tools/server && python3 main.py

# All-in-one dashboard (monitor + device discovery + UCI commands)
python3 tools/dashboard.py
python3 tools/dashboard.py --no-monitor       # if monitor.py already running separately
python3 tools/dashboard.py --iface wpan0      # explicit Thread interface

# OTBR container logs (debugging)
docker logs otbr

# Stop OTBR
docker compose -f tools/otbr/docker-compose.yml down
```

## OTA Firmware Updates

MCUmgr SMP over UDP + MCUboot bootloader. Signed images uploaded over Thread IPv6:
```bash
# Upload new firmware
./tools/scripts/ota_update.sh <device-ipv6-addr> firmware/build/<board>/firmware/zephyr/zephyr.signed.bin

# Bump version before building update (firmware/VERSION)
```

Anchor (nRF52840) and XIAO BLE: dual-slot with swap + rollback over Thread. Tag (DWM3001CDK): single-slot MCUboot with serial recovery — hold button (P0.02) during reset, then upload via `mcumgr --conntype serial`. Flash all boards using `merged.hex` (not `zephyr.hex`) to include MCUboot + signed app.

Tag serial recovery:
```bash
# Hold P0.02 button, reset board, then:
mcumgr --conntype serial --connstring dev=/dev/ttyACM3,baud=115200 image upload firmware/build/decawave_dwm3001cdk/firmware/zephyr/zephyr.signed.bin
```

## Distance Calibration

DS-TWR measurements have a systematic offset from antenna delays and environmental factors. Calibrate by placing anchor and tag exactly 1 meter apart:

```bash
# 1. Ensure ranging is active and producing measurements
python3 tools/scripts/uwb_tool.py /dev/ttyACM1 start
# 2. Wait a few seconds for stable measurements, then calibrate
python3 tools/scripts/uwb_tool.py /dev/ttyACM1 calibrate
# 3. Save offset to flash
python3 tools/scripts/uwb_tool.py /dev/ttyACM1 save
```

The `calibrate` command reads the current distance, computes `offset = 1000 - raw_mm`, and stores it as `calibration_offset_mm` (signed int16_t, ±32m range). The offset is applied to all subsequent distance measurements. Factory reset clears the calibration.

## Architecture

Shared firmware source builds for all boards; board-specific config in `firmware/boards/*.conf` and `*.overlay`. Role (anchor/tag) is set via Kconfig defaults per board but overridable at runtime via UCI + NVS.

**Firmware modules** (all in `firmware/src/`):
- `main.c` — boot sequence: device_config → thread_coap → uci_coap → uwb_manager → uci_uart → autostart
- `uwb_manager.c/h` — DS-TWR ranging loop (interrupt-driven, priority 0), multi-anchor peer list, auto-discovery, start/stop/status API
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

Channel 15, PAN ID 0xABCD (43981), network key `00:11:22:33:44:55:66:77:88:99:aa:bb:cc:dd:ee:ff`. All devices, `tools/otbr/otbr-env.list`, and `tools/scripts/otbr_setup.sh` must match.

## Testing

Automated integration test suite that exercises all UCI commands over live hardware:

```bash
# Full suite (devices must be powered on, Thread network active)
python3 tools/scripts/test_firmware.py -v

# Quick smoke test (non-destructive, ~30s)
python3 tools/scripts/test_firmware.py -v -k TestInfo

# Discover devices only (verify Thread connectivity)
python3 tools/scripts/test_firmware.py --discover

# Run via UART instead of CoAP
python3 tools/scripts/test_firmware.py --transport uart -v

# Explicit device addresses (skip auto-discovery)
python3 tools/scripts/test_firmware.py --anchor-ip <ipv6> --tag-ip <ipv6> -v
```

**Run this test suite after architecture changes or firmware refactors** to verify nothing is broken. Destructive tests (TestConfigPersistence, TestRoleSwap) factory-reset devices in tearDown.

**Add new tests** when new UCI commands or testable features are developed, or when edge cases are discovered during debugging.

## Meta: Maintaining This File

When you discover useful commands for controlling, debugging, or interacting with the hardware (e.g. `nrfutil device reset`, J-Link tricks, Thread CLI commands, CoAP queries), add them to the appropriate section of this file so they're available in future sessions.
