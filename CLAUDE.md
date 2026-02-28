# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

UWB Mesh Tracker — real-time indoor positioning using Ultra-Wideband (DS-TWR) ranging over a Thread mesh network. Two nRF boards perform UWB ranging; distance measurements are sent via CoAP over Thread to a Linux host.

## Build & Flash

The nRF Connect SDK lives at `~/ncs/v3.2.2` (installed via nrfutil, NOT a local copy). The build script handles all toolchain env setup.

```bash
# Build only
bash firmware/build.sh nrf52840dk/nrf52840           # anchor
bash firmware/build.sh decawave_dwm3001cdk           # tag

# Build + flash (single board connected)
bash firmware/build.sh nrf52840dk/nrf52840 --flash
bash firmware/build.sh decawave_dwm3001cdk --flash

# Clean rebuild
bash firmware/build.sh nrf52840dk/nrf52840 --clean --flash
```

**When both boards are connected**, `west flash` refuses to guess which to target. Flash manually:
```bash
cd ~/ncs/v3.2.2
west flash --build-dir ~/Projects/capstone/firmware/build/nrf52840dk-nrf52840 --dev-id 1050222631   # anchor
west flash --build-dir ~/Projects/capstone/firmware/build/decawave_dwm3001cdk --dev-id 760206311    # tag
```

Board targets must include the SoC qualifier: `nrf52840dk/nrf52840` not `nrf52840dk`.

## Hardware

| Device | Role | Serial/dev-id | UART |
|---|---|---|---|
| nRF52840 DK + DWM3000EVB | Anchor (DS-TWR responder) | 1050222631 | /dev/ttyACM1 |
| DWM3001CDK | Tag (DS-TWR initiator) | 760206311 | /dev/ttyACM3 |
| nRF52840 USB Dongle | Thread RCP | — | /dev/ttyACM0 |

## UCI Device Configuration

Binary protocol over UART (`scripts/uwb_tool.py`), not a text shell:
```bash
python3 scripts/uwb_tool.py /dev/ttyACM1 info
python3 scripts/uwb_tool.py /dev/ttyACM1 status
python3 scripts/uwb_tool.py /dev/ttyACM1 stop
python3 scripts/uwb_tool.py /dev/ttyACM1 start
python3 scripts/uwb_tool.py /dev/ttyACM3 set-interval 500
python3 scripts/uwb_tool.py /dev/ttyACM1 save          # persist to NVS flash
python3 scripts/uwb_tool.py /dev/ttyACM1 factory-reset
```

Frame format: `[0xAA][CMD][LEN][PAYLOAD][CRC8]` → `[0xBB][CMD][STATUS][LEN][PAYLOAD][CRC8]`

## Monitoring

```bash
# Start Thread on Linux host (once, as root)
sudo bash scripts/thread_dongle_setup.sh

# Live CoAP monitor (distance + tag events)
python3 monitor.py

# Full CoAP server with SQLite storage
cd server && python3 main.py
```

## OTA Firmware Updates

MCUmgr SMP over UDP + MCUboot bootloader. Signed images uploaded over Thread IPv6:
```bash
# Install mcumgr CLI (one-time)
go install github.com/apache/mynewt-mcumgr-cli/mcumgr@latest

# Upload new firmware
./scripts/ota_update.sh <device-ipv6-addr> firmware/build/<board>/firmware/zephyr/zephyr.signed.bin

# Bump version before building update (firmware/VERSION)
```

Anchor (nRF52840): dual-slot with swap + rollback over Thread. Tag (DWM3001CDK): single-slot MCUboot with serial recovery — hold button (P0.02) during reset, then upload via `mcumgr --conntype serial`. Flash both boards using `merged.hex` (not `zephyr.hex`) to include MCUboot + signed app.

Tag serial recovery:
```bash
# Hold P0.02 button, reset board, then:
mcumgr --conntype serial --connstring dev=/dev/ttyACM3,baud=115200 image upload firmware/build/decawave_dwm3001cdk/firmware/zephyr/zephyr.signed.bin
```

## Architecture

Shared firmware source builds for both boards; board-specific config in `firmware/boards/*.conf` and `*.overlay`. Role (anchor/tag) is set via Kconfig defaults per board but overridable at runtime via UCI + NVS.

**Firmware modules** (all in `firmware/src/`):
- `main.c` — boot sequence: device_config → thread_coap → uci_coap → uwb_manager → uci_uart → autostart
- `uwb_manager.c/h` — DS-TWR ranging loop (interrupt-driven, priority 0), start/stop/status API
- `thread_coap.c/h` — Thread join + CoAP POST `/distance` (anchor) and `/event` (tag) to multicast
- `device_config.c/h` — NVS-backed persistent config (role, addr, interval, server, autostart)
- `uci.c/h` — UCI binary protocol parser and command dispatch (mutex-protected)
- `uci_uart.c` — UART transport (ISR RX ring buffer → state machine → response TX)
- `uci_coap.c` — CoAP transport (OT CoAP server, POST /cmd, remote UCI)

**Config layering**: Kconfig defaults → board `.conf` overrides → NVS saved values (loaded by device_config_init at boot)

## Key Gotchas

- `CONFIG_UART_INTERRUPT_DRIVEN=y` is required in prj.conf — shell used to pull it in, but shell is now disabled to save RAM
- `CONFIG_OPENTHREAD_PANID` is type `int` — use decimal `43981` not hex `0xABCD`
- DW3000 driver API differences from Qorvo examples: `dwt_readrxtimestamp(buf, DWT_IP_M)`, `dwt_getframelength(NULL)`, don't redefine `DWT_TIME_UNITS`/`FCS_LEN`
- The DW3000 Zephyr driver module is at `../zephyr-dw3000-decadriver` (sibling of `firmware/`, in .gitignore)
- Tag (DWM3001CDK) uses DW3720 chip variant; anchor (nRF52840 DK + DWM3000EVB) uses DW3000
- UWB thread uses `k_sched_lock()` around timing-critical TX/RX sequences; preamble timeout is 0 for anchor's FINAL wait
- Stop/start ranging requires draining semaphores and clearing DW3000 status bits to avoid stale state
- Use pyserial scripts to read UART output (screen/cat unreliable after flash)

## Thread Network

Channel 15, PAN ID 0xABCD (43981), network key `00:11:22:33:44:55:66:77:88:99:aa:bb:cc:dd:ee:ff`. All devices and `scripts/thread_dongle_setup.sh` must match.
