# UWB Mesh Tracker — Project Status

_Last updated: 2026-03-02_

---

## What's Working

### UWB Ranging
- DS-TWR (Double-Sided Two-Way Ranging) between tag and anchor
- Interrupt-driven — no spin-loops, CPU free between events
- ~5 ranging cycles per second (200ms default interval, configurable at runtime via UCI)
- 100% success rate, no dropped cycles
- Distance accuracy ~±3cm at short range (limited by uncalibrated antenna delay)
- DW3000/DW3720 TXLED/RXLED active on both modules as visual indicators (via `dwt_setleds`)
- Runtime stop/start — ranging can be paused and resumed without reboot

### Thread Networking
- Both devices join a Thread network and connect automatically
- Anchor promotes to Router role, tag stays as Leader or Router
- Thread stack running and network forms on boot
- Linux host connectivity via nRF52840 USB dongle running ot-daemon (wpan0 interface)
- Native ping6 to both boards from Linux: ~15ms RTT, 0% packet loss

### CoAP Data Reporting
- Anchor sends POST `/distance` (12-byte binary) on every ranging cycle to `ff03::1` multicast
- Tag sends POST `/event` (6-byte binary) for each UWB packet (POLL_TX, RESP_RX, FINAL_TX, NO_RESP)
- `monitor.py` — lightweight CoAP listener that displays live distance and event data
- `server/` — full aiocoap CoAP server with SQLite storage backend

### UCI Command Interface
- Binary UCI protocol for device configuration and control
- **Two transports**: UART (serial) and CoAP over Thread (remote, no physical access needed)
- Python host tool (`scripts/uwb_tool.py`) supports both — auto-detects `coap://` prefix
- 11 commands: info, status, set-role, set-addr, set-interval, set-server, start, stop, save, factory-reset, enter-bootloader
- Persistent config via NVS flash — survives reboots
- Devices auto-start ranging on boot with saved config (configurable)
- Thread-safe mutex protects UCI processing when both transports are active simultaneously

### Remote UCI over CoAP
- OpenThread native CoAP server on port 5683, resource POST `/cmd`
- UCI commands sent as CoAP payload: `[CMD][LEN][PAYLOAD]` (no sync/CRC — CoAP handles framing and integrity)
- UCI response returned in CoAP response: `[CMD][STATUS][LEN][PAYLOAD]`
- Supports both CON (confirmable, ACK reply) and NON (non-confirmable) CoAP request types
- No additional threads or sockets — handler runs in existing OpenThread event loop
- Negligible memory impact (~0% change on both boards)

### OTA Firmware Updates
- MCUmgr SMP server over UDP (port 1337) — works natively over Thread IPv6
- MCUboot secure bootloader verifies signed images before booting
- Anchor (nRF52840, 1024KB): dual-slot MCUboot with swap — supports rollback on failed update
- Tag (DWM3001CDK, 512KB): single-slot MCUboot with **serial recovery** — hold button (P0.02) during reset to enter bootloader, then upload via `mcumgr` over UART (no OTA over Thread — 512KB flash too small for two app slots)
- XIAO BLE (nRF52840, 1024KB): **app-only** with stock Adafruit UF2 bootloader — no MCUboot, no OTA. Flash via UF2 drag-and-drop (`zephyr.uf2`): double-tap RST, drag file to USB drive
- No source code changes — SMP UDP transport auto-initializes from Kconfig
- Signed firmware images generated automatically by build (`zephyr.signed.bin`)
- Host-side: `mcumgr` CLI uploads images over UDP, `scripts/ota_update.sh` convenience wrapper
- Tested: v1.0.0 → v1.0.1 OTA update on anchor over Thread — upload ~49s at ~6.9 KiB/s, MCUboot swap + confirm verified
- **Workaround**: nRF SDK v3.2.2 `pm_sysflash.h` bug — only checks `CONFIG_SINGLE_APPLICATION_SLOT` (MCUboot-only), not `CONFIG_MCUBOOT_BOOTLOADER_MODE_SINGLE_APP` (app-level). Fixed via `zephyr_compile_definitions()` in CMakeLists.txt

---

## Architecture

```
┌─────────────────────────────┐         ┌─────────────────────────────┐
│   DWM3001CDK (TAG)          │         │  nRF52840 DK + DWM3000EVB   │
│   nRF52833                  │   UWB   │  (ANCHOR 0x0001) nRF52840   │
│                             │◄───────►│                             │
│  Role: DS-TWR Initiator     │  radio  │  Role: DS-TWR Responder     │
│  - Sends POLL               │         │  - Receives POLL            │
│  - Receives RESP            │         │  - Sends RESP               │
│  - Sends FINAL              │         │  - Receives FINAL           │
│  - Sleeps interval, repeat  │         │  - Computes distance        │
│  - POST /event via CoAP     │         │  - POST /distance via CoAP  │
│                             │         │                             │
│  Thread role: Leader/Router │         │  Thread role: Router        │
│  UCI: UART /dev/ttyACM3     │         │  UCI: UART /dev/ttyACM1     │
│  UCI: CoAP POST /cmd        │         │  UCI: CoAP POST /cmd        │
└─────────────────────────────┘         └──────────┬──────────────────┘
                                                   │ Thread (802.15.4)
                                                   │ CoAP POST
                                                   ▼
                                        ┌─────────────────────────────┐
                                        │  nRF52840 USB Dongle (RCP)  │
                                        │  ot-daemon → wpan0          │
                                        └──────────┬──────────────────┘
                                                   │ IPv6
                                                   ▼
                                        ┌─────────────────────────────┐
                                        │  Linux Host                 │
                                        │  monitor.py (live view)     │
                                        │  server/main.py (SQLite)    │
                                        │  scripts/uwb_tool.py (UCI)  │
                                        │   ↳ serial or coap://[addr] │
                                        └─────────────────────────────┘
```

### Firmware Internal Architecture

```
                    ┌───────────────────────────────┐
                    │         main.c                 │
                    │  device_config_init()           │
                    │  thread_coap_init()             │
                    │  uci_coap_init()                │
                    │  uwb_manager_init()             │
                    │  uci_uart_init()                │
                    │  uwb_manager_start() (if auto)  │
                    └──┬──────────┬──────────┬───────┘
                       │          │          │
          ┌────────────▼──┐  ┌───▼────┐  ┌──▼───────────┐
          │ uwb_manager   │  │ thread │  │ uci_uart     │
          │ (priority 0)  │  │ _coap  │  │ (priority 8) │
          │               │  │        │  │              │
          │ DS-TWR loop   │  │ CoAP   │  │ UART RX ISR  │
          │ DW3000 IRQ    │──│ POST   │  │ state machine│
          │ distance_cb() │  │ queue  │  │ → uci.c ◄────┼── uci_coap
          └───────────────┘  └────────┘  └──────────────┘   (OT CoAP
                                              │              POST /cmd)
                                         ┌────▼──────────┐
                                         │ device_config  │
                                         │ NVS flash      │
                                         │ (settings API) │
                                         └───────────────┘
```

---

## Hardware

| Device | Role | Serial | UART | Flash Used | RAM Used |
|---|---|---|---|---|---|
| nRF52840 DK + DWM3000EVB shield | Anchor 0x0001 / DS-TWR Responder | 1050222631 | /dev/ttyACM1 | 71% (348KB/491KB) | 54% (141KB/256KB) |
| DWM3001CDK | Tag 0x0100 / DS-TWR Initiator | 760206311 | /dev/ttyACM3 | 70% (331KB/475KB) | 93% (122KB/128KB) |
| XIAO BLE + DWM3000EVB (wires) | Tag 0x0200 / DS-TWR Initiator | — | TBD | 77% (358KB/464KB) | 57% (146KB/256KB) |
| nRF52840 USB Dongle | Thread RCP (border interface) | — | /dev/ttyACM0 | — | — |

### XIAO BLE → DWM3000EVB Wiring

No Arduino header — connected via jumper wires from the DWM3000EVB Arduino headers to XIAO breakout pins.

| DWM3000 Signal | Module Pin | EVB Arduino Pin | XIAO Pin | XIAO GPIO | Notes |
|---|---|---|---|---|---|
| SPICLK | 20 | SPI header (J4) SCK | D8 | P1.13 | SPI2 clock |
| SPIMOSI | 18 | SPI header (J4) MOSI | D10 | P1.15 | SPI2 master-out |
| SPIMISO | 19 | SPI header (J4) MISO | D9 | P1.14 | SPI2 master-in |
| SPICSn | 17 | D10 | D0 | P0.02 | Chip select (active low) |
| GPIO8/IRQ | 22 | D8 | D1 | P0.03 | Interrupt (active high) |
| RSTn | 3 | D7 | D2 | P0.28 | Reset (active low) |
| WAKEUP | 2 | D9 | D3 | P0.29 | Wakeup (active high) |
| GPIO5/SPIPOL | 10 | D1 | D4 | P0.04 | SPI polarity select |
| GPIO6/SPIPHA | 9 | D0 | D5 | P0.05 | SPI phase select |
| VDD3V3 | 6,7 | 3.3V (J6) | 3V3 | — | Power |
| VSS | 8,16,21,23,24 | GND (J6) | GND | — | Ground |

UART0 (D6=TX, D7=RX) available for UCI via USB-C CDC ACM. App-only build with UF2 flashing (no MCUboot/OTA).

---

## UCI Command Reference

```
python3 scripts/uwb_tool.py <target> <command> [args...]
```

Target can be a serial port or a CoAP URI:
- `/dev/ttyACM1` — UART (local)
- `coap://[fdde:ad00:beef:0:a]` — CoAP over Thread (remote)

| Command | Description | Example |
|---|---|---|
| `info` | Firmware version, role, address, state | `uwb_tool.py /dev/ttyACM1 info` |
| `status` | Running state, last distance, range count | `uwb_tool.py coap://[fd00::1] status` |
| `set-role <anchor\|tag>` | Set node role (save + reboot to apply) | `uwb_tool.py /dev/ttyACM3 set-role tag` |
| `set-addr <0xNNNN>` | Set UWB short address | `uwb_tool.py /dev/ttyACM3 set-addr 0x0200` |
| `set-interval <ms>` | Set ranging interval (50–10000 ms) | `uwb_tool.py coap://[fd00::1] set-interval 500` |
| `set-server <ipv6> <port>` | Set CoAP server address | `uwb_tool.py /dev/ttyACM1 set-server ff03::1 5683` |
| `start` | Start ranging | `uwb_tool.py coap://[fd00::1] start` |
| `stop` | Stop ranging | `uwb_tool.py /dev/ttyACM1 stop` |
| `save` | Persist config to NVS flash | `uwb_tool.py /dev/ttyACM1 save` |
| `factory-reset` | Erase config and reboot | `uwb_tool.py /dev/ttyACM1 factory-reset` |
| `enter-bootloader` | Reboot into MCUboot serial recovery | `uwb_tool.py /dev/ttyACM3 enter-bootloader` |

UCI over UART: `[0xAA][CMD][LEN][PAYLOAD][CRC8]` → `[0xBB][CMD][STATUS][LEN][PAYLOAD][CRC8]`

UCI over CoAP: POST `/cmd` payload `[CMD][LEN][PAYLOAD]` → response `[CMD][STATUS][LEN][PAYLOAD]`

### Thread Network Utilities

```bash
sudo ot-ctl state              # Current role (leader/router/child)
sudo ot-ctl ipaddr             # Dongle's IPv6 addresses
sudo ot-ctl neighbor table     # Devices on the mesh (RLOC16, mode, age)
sudo ot-ctl router table       # All routers in the network
ping6 -c 3 -I wpan0 ff03::1   # Ping all mesh nodes (discover addresses)
```

Use the mesh-local address from `neighbor table` or `ping6` output with the CoAP transport:
```bash
python3 scripts/uwb_tool.py coap://[<mesh-local-addr>] info
```

### MCUmgr Commands

```bash
# --- Anchor OTA update (over Thread, dual-slot with swap) ---
mcumgr --conntype udp --connstring=[<ipv6-addr>]:1337 image upload firmware.signed.bin
mcumgr --conntype udp --connstring=[<ipv6-addr>]:1337 image list
mcumgr --conntype udp --connstring=[<ipv6-addr>]:1337 image confirm <hash>
mcumgr --conntype udp --connstring=[<ipv6-addr>]:1337 reset

# --- Tag DWM3001CDK serial recovery (over UART, single-slot) ---
# Enter recovery first:
python3 scripts/uwb_tool.py /dev/ttyACM3 enter-bootloader   # or hold P0.02 during reset
# Then upload:
mcumgr --conntype serial --connstring dev=/dev/ttyACM3,baud=115200 image upload firmware.signed.bin

# --- XIAO BLE (app-only, UF2 drag-and-drop) ---
# Double-tap RST to enter UF2 bootloader, drag zephyr.uf2 to USB drive

# --- Common ---
mcumgr --conntype udp --connstring=[<ipv6-addr>]:1337 echo hello    # Test SMP connectivity
mcumgr --conntype udp --connstring=[<ipv6-addr>]:1337 image list    # Show installed images
```

---

## Scripts

| Script | Purpose | Usage |
|---|---|---|
| `firmware/build.sh` | Build and flash firmware | `bash firmware/build.sh nrf52840dk/nrf52840 --flash --clean` |
| `scripts/thread_dongle_setup.sh` | Start ot-daemon + configure Thread network | `bash scripts/thread_dongle_setup.sh` |
| `scripts/uwb_tool.py` | UCI device configuration tool | `python3 scripts/uwb_tool.py /dev/ttyACM1 info` |
| `scripts/ota_update.sh` | OTA firmware update over Thread | `./scripts/ota_update.sh <ipv6-addr> firmware.signed.bin` |
| `monitor.py` | Live CoAP distance + event monitor | `python3 monitor.py` |
| `server/main.py` | Full CoAP server with SQLite | `cd server && python3 main.py` |

---

## Key Files

| File | Purpose |
|---|---|
| `firmware/src/main.c` | Boot, init, wiring (config → thread → uci_coap → uwb → uci_uart) |
| `firmware/src/leds.h` | LED abstraction (wraps DK library) |
| `firmware/src/uwb_manager.c/h` | DS-TWR ranging (interrupt-driven, start/stop/status) |
| `firmware/src/thread_coap.c/h` | Thread networking + CoAP POST |
| `firmware/src/device_config.c/h` | NVS-backed runtime config (role, addr, interval, server) |
| `firmware/src/uci.c/h` | UCI binary protocol handler + command dispatch (mutex-protected) |
| `firmware/src/uci_uart.c` | UART transport (ISR RX, state machine, response TX) |
| `firmware/src/uci_coap.c` | CoAP transport (OT CoAP server, POST /cmd, remote UCI) |
| `firmware/Kconfig` | Custom Kconfig options (role, addr, interval, server, etc.) |
| `firmware/prj.conf` | Base project config (Thread, CoAP, DW3000, NVS, reboot — portable across SDKs) |
| `firmware/boards/*.conf` | Per-board overrides (role, chip variant, logging, nRF-specific Kconfig) |
| `firmware/boards/*.overlay` | Per-board device tree (SPI, GPIO, IRQ pins) |
| `firmware/sysbuild.conf` | MCUboot bootloader enable (sysbuild) |
| `firmware/sysbuild_dwm3001cdk.conf` | DWM3001CDK sysbuild overlay (single-slot MCUboot) |
| `firmware/sysbuild/mcuboot.conf` | MCUboot config (min partition size) |
| `firmware/sysbuild/mcuboot_dwm3001cdk/` | MCUboot serial recovery config + DT overlay for tag |
| `firmware/VERSION` | Image version for MCUboot signing |

---

## What Still Needs Work

1. **Multiple anchors** — currently hardcoded for one tag (`0x0100`) and one anchor (`0x0001`). Scaling requires addressing, scheduling, and possibly a coordinator role

2. **Tag-to-tag ranging** — future goal for tags to reference other tags, requires protocol changes

4. **Antenna delay calibration** — `TX_ANT_DLY`/`RX_ANT_DLY` (16385) are defaults. Calibration at a known distance would improve accuracy from ~±3cm to ~±10mm

5. **Web dashboard** — visualize distance measurements in real time (FastAPI/WebSocket planned in server/)
