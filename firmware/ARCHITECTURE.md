# Firmware Architecture

UWB Mesh Tracker firmware — shared source builds for anchor (nRF52840 DK + DWM3000EVB) and tag (DWM3001CDK). Role selected at compile time via Kconfig, overridable at runtime via UCI + NVS.

## Source Files

```
firmware/src/
  main.c             Boot sequence, distance callback, LED blink
  device_config.c/h  NVS-backed persistent configuration
  thread_coap.c/h    Thread network + CoAP distance/event reporting
  uci_coap.c         UCI commands over CoAP (POST /cmd)
  uwb_manager.c/h    DW3000 init, DS-TWR ranging loops
  uci.c/h            UCI command processor (transport-agnostic)
  uci_uart.c         UCI UART transport (ISR RX + state machine)
  leds.h             LED abstraction (wraps dk_buttons_and_leds.h)
```

---

## Boot Sequence

`main()` in `main.c` runs these steps in order. Any step marked **fatal** returns early and halts the application.

```
main()
 │
 ├─ 1. device_config_init()                          [FATAL on failure]
 │     ├─ load_defaults()         ← Kconfig values → g_config
 │     ├─ settings_subsys_init()  ← initialize NVS backend
 │     └─ settings_load()         ← NVS overrides → g_config
 │
 ├─ 2. dk_leds_init()                                [warn on failure, continue]
 │
 ├─ 3. thread_coap_init()                            [FATAL on failure]
 │     ├─ zsock_inet_pton()       ← parse g_config.server_addr
 │     ├─ coap_init()             ← Zephyr CoAP library
 │     ├─ k_work_queue_start()    ← dedicated CoAP work queue (prio 5)
 │     ├─ k_work_init() × 2      ← distance + event work items
 │     ├─ openthread_state_changed_callback_register()
 │     └─ openthread_run()        ← start Thread stack
 │
 ├─ 4. uci_coap_init()                              [warn on failure, continue]
 │     ├─ openthread_get_default_instance()
 │     ├─ otCoapAddResource()     ← register POST /cmd
 │     └─ otCoapStart()           ← listen on port 5683
 │
 ├─ 5. uwb_manager_init()                           [FATAL on failure]
 │     ├─ dw3000_hw_init()        ← GPIO + SPI pins
 │     ├─ dw3000_hw_reset()       ← hardware reset pulse
 │     ├─ dwt_probe()             ← detect chip on SPI (2 MHz)
 │     ├─ dw3000_spi_speed_fast() ← switch to fast SPI
 │     ├─ dwt_initialise()        ← chip init + load trim values
 │     ├─ dwt_checkidlerc()       ← poll until chip is idle (200ms timeout)
 │     ├─ dwt_configure()         ← PHY: ch5, 6.8Mbps, 128-sym preamble
 │     ├─ dwt_configuretxrf()     ← TX power
 │     ├─ dwt_otpread()           ← read factory antenna delay from OTP
 │     ├─ dwt_setrxantennadelay() ← apply RX delay (OTP or fallback 16366)
 │     ├─ dwt_settxantennadelay() ← apply TX delay (OTP or fallback 16366)
 │     ├─ dwt_setlnapamode()      ← enable LNA + PA
 │     ├─ dwt_setleds()           ← enable on-chip TX/RX LEDs
 │     ├─ dwt_setcallbacks()      ← register TX/RX/TO/ERR callbacks
 │     ├─ dwt_setinterrupt()      ← enable IRQ mask
 │     └─ dw3000_hw_init_interrupt() ← connect GPIO IRQ to dwt_isr
 │
 ├─ 6. uwb_manager_set_distance_cb(on_distance_measured)
 │
 ├─ 7. uci_uart_init()                              [FATAL on failure]
 │     ├─ DEVICE_DT_GET(zephyr_console)
 │     ├─ uart_irq_callback_set() ← install ISR
 │     ├─ uart_irq_rx_enable()    ← start receiving bytes
 │     └─ k_thread_create()       ← "uci_uart" thread (prio 8)
 │
 ├─ 8. if (g_config.autostart):
 │     └─ uwb_manager_start()     ← creates "uwb" thread (prio 0), sets atomic flag
 │
 └─ 9. dk_set_led_on(LED1)       ← app running indicator
       loop: k_msleep(5000)       ← main thread idle
```

---

## Thread Architecture

All threads are created at boot. The UWB thread is created on first `uwb_manager_start()` and never destroyed — stop/start is controlled by an atomic flag.

| Thread | Priority | Stack | Role |
|--------|----------|-------|------|
| `main` | 0 (cooperative) | 4096 B | Boot sequence, then idle sleep |
| `uwb` | 0 (cooperative) | 2048 B | DS-TWR ranging loop |
| `coap_workq` | 5 (preemptible) | 2048 B | Async CoAP POST sends |
| `uci_uart` | 8 (preemptible) | 1536 B | UART RX state machine |
| `sysworkq` | varies | 8192/2048 B | Zephyr system work queue |
| `ot` | (internal) | — | OpenThread stack |
| `log` | 1 | — | Deferred log processing |
| `mcumgr_udp` | 5 | 2048 B | SMP OTA transport (anchor) |

### Priority Design

- **Priority 0 (cooperative)**: UWB thread — cannot be preempted during timing-critical DW3000 register sequences. Additionally uses `k_sched_lock()` around TX delay calculation + `dwt_starttx()` to prevent any context switch between reading timestamps and programming the delayed TX.
- **Priority 1**: Log thread — processes deferred log messages without interfering with ranging.
- **Priority 5**: CoAP work queue + MCUmgr — network I/O that can tolerate latency.
- **Priority 8**: UCI UART — lowest priority; command processing is infrequent and latency-tolerant.

---

## Interrupt Architecture

```
DW3000 IRQ pin (GPIO)
  │
  ├─ GPIO ISR fires
  │   └─ schedules k_work
  │       └─ k_work handler calls dwt_isr()
  │           └─ dwt_isr() reads DW3000 SYS_STATUS register
  │               ├─ TX frame sent    → cb_tx_done() → k_sem_give(&sem_tx_done)
  │               ├─ RX frame OK      → cb_rx_ok()   → k_sem_give(&sem_rx_ok)
  │               ├─ RX timeout       → cb_rx_to()   → k_sem_give(&sem_rx_to)
  │               └─ RX error         → cb_rx_err()  → k_sem_give(&sem_rx_err)
  │
  └─ UWB thread blocked on wait_for_event()
      └─ polls all 4 semaphores (sem_rx_ok, sem_tx_done, sem_rx_to, sem_rx_err)
          with K_NO_WAIT in a tight loop, sleeping 100 µs between polls
```

The `wait_for_event()` function accepts a timeout. If none of the semaphores are signaled before the deadline, it returns `EVT_RX_TO` (treated as timeout).

### UART ISR

```
UART RX interrupt
  │
  └─ uart_isr_callback()
      ├─ uart_fifo_read() → bytes into rx_ring[] (128-byte ring buffer)
      └─ k_sem_give(&rx_sem) → wakes uci_uart thread
```

The `uci_uart` thread calls `read_byte()`, which blocks on `rx_sem` when the ring buffer is empty.

---

## DS-TWR Ranging Protocol

Double-Sided Two-Way Ranging. The anchor computes distance; the tag just provides its timestamps.

### Message Flow

```
     Tag (initiator)                          Anchor (responder)
     ───────────────                          ──────────────────

  ┌─ POLL TX ──────────────────────────────►  POLL RX
  │  (fc=0x21, src_addr=tag_addr)              │
  │                                            ├─ validate frame
  │                                            ├─ read poll_rx_ts
  │                                            ├─ k_sched_lock()
  │                                            ├─ compute delayed TX time
  │                                            │  (poll_rx + 3500µs)
  │                                            ▼
  │  RESP RX ◄────────────────────────────── RESP TX (delayed)
  │   │                                       (fc=0x10, delayed by 3500 µs)
  │   ├─ validate frame                        │
  │   ├─ read poll_tx_ts, resp_rx_ts           ├─ arm RX for FINAL
  │   ├─ k_sched_lock()                        │  (timeout: 5000 µs)
  │   ├─ compute final_tx_ts                   │
  │   │  (resp_rx + 3500µs)                    │
  │   ▼                                        │
  └─ FINAL TX (delayed) ──────────────────►  FINAL RX
     (fc=0x23, carries 3 timestamps:           │
      poll_tx_ts, resp_rx_ts, final_tx_ts)     ├─ read resp_tx_ts, final_rx_ts
                                               ├─ extract tag's 3 timestamps
                                               ▼
                                             COMPUTE DISTANCE
                                               Ra = resp_rx - poll_tx   (tag)
                                               Rb = final_rx - resp_tx  (anchor)
                                               Da = final_tx - resp_rx  (tag)
                                               Db = resp_tx - poll_rx   (anchor)

                                               ToF = (Ra·Rb - Da·Db) / (Ra + Rb + Da + Db)
                                               distance = ToF × c
```

### Frame Formats

All frames use IEEE 802.15.4 short format with PAN ID `0xDECA`:

| Frame | Bytes | Function Code | Extra Fields |
|-------|-------|---------------|--------------|
| POLL | 12 | `0x21` | `[10-11]` tag source address (LE uint16) |
| RESP | 14 | `0x10` | — |
| FINAL | 22 | `0x23` | `[10-13]` poll_tx_ts, `[14-17]` resp_rx_ts, `[18-21]` final_tx_ts (LE uint32 each, truncated from 40-bit) |

Common header (bytes 0–9): `[0x41, 0x88, seq, 0xCA, 0xDE, dst[0..3], fc]`

### Timing Constants

| Parameter | Value | Purpose |
|-----------|-------|---------|
| `POLL_RX_TO_RESP_TX_DLY_UUS` | 3500 µs | Anchor delay before sending RESP |
| `RESP_TX_TO_FINAL_RX_DLY_UUS` | 1000 µs | Anchor arms RX this long after RESP TX |
| `FINAL_RX_TIMEOUT_UUS` | 5000 µs | Anchor gives up waiting for FINAL |
| `POLL_TX_TO_RESP_RX_DLY_UUS` | 3300 µs | Tag arms RX this long after POLL TX |
| `RESP_RX_TO_FINAL_TX_DLY_UUS` | 3500 µs | Tag delay before sending FINAL |
| `RESP_RX_TIMEOUT_UUS` | 1000 µs | Tag gives up waiting for RESP |
| `PRE_TIMEOUT` | 64 symbols | Preamble detection timeout (tag only) |

### PHY Configuration

- Channel 5 (6489.6 MHz)
- Data rate: 6.8 Mbps
- Preamble: 128 symbols, PAC 8
- Preamble code: 9 (TX and RX)
- SFD: non-standard 8-symbol
- STS: off
- Antenna delay: read from OTP at init (fallback: 16366)

---

## Anchor State Machine

```
                            ┌──────────────────────────────┐
                            │                              │
                     uwb_running=0                  uwb_running=0
                            │                              │
                            ▼                              │
                      ┌───────────┐                        │
       ┌──────────────│   IDLE    │◄────────────────┐      │
       │              └─────┬─────┘                 │      │
       │               uwb_running=1                │      │
       │                    │                       │      │
       │          drain_semaphores()                 │      │
       │          clear DW3000 status                │      │
       │                    │                       │      │
       │                    ▼                       │      │
       │            ┌───────────────┐               │      │
       │            │  WAIT POLL    │               │      │
       │            │  (RX enabled, │               │      │
       │            │   no timeout) │               │      │
       │            └───────┬───────┘               │      │
       │                    │ EVT_RX_OK             │      │
       │                    │ + valid POLL frame     │      │
       │                    ▼                       │      │
       │            ┌───────────────┐    DWT_ERROR  │      │
       │            │  SEND RESP    ├───────────────┘      │
       │            │  (delayed TX, │                      │
       │            │   sched_lock) │                      │
       │            └───────┬───────┘                      │
       │                    │ EVT_TX_DONE                  │
       │                    ▼                              │
       │            ┌───────────────┐    EVT_RX_TO/ERR     │
       │            │  WAIT FINAL   ├──────────────────────┘
       │            │  (5000 µs     │
       │            │   timeout)    │
       │            └───────┬───────┘
       │                    │ EVT_RX_OK
       │                    │ + valid FINAL frame
       │                    ▼
       │            ┌───────────────┐
       │            │   COMPUTE     │
       │            │   DISTANCE    │
       │            │               │
       │            │  Ra,Rb,Da,Db  │
       │            │  → ToF → m   │
       │            └───────┬───────┘
       │                    │
       │                    │ distance_cb()
       │                    │   └─ blink LED2
       │                    │   └─ thread_coap_send_distance()
       │                    │
       └────────────────────┘   (loop back to WAIT POLL)
```

---

## Tag State Machine

```
                            ┌──────────────────────────────┐
                            │                              │
                     uwb_running=0                  uwb_running=0
                            │                              │
                            ▼                              │
                      ┌───────────┐                        │
       ┌──────────────│   IDLE    │◄────────────────┐      │
       │              └─────┬─────┘                 │      │
       │               uwb_running=1                │      │
       │                    │                       │      │
       │          drain_semaphores()                 │      │
       │          clear DW3000 status                │      │
       │                    │                       │      │
       │                    ▼                       │      │
       │            ┌───────────────┐               │      │
       │            │  SEND POLL    │               │      │
       │            │  (immediate   │               │      │
       │            │   TX + arm RX)│               │      │
       │            └───────┬───────┘               │      │
       │                    │ EVT_TX_DONE           │      │
       │                    ▼                       │      │
       │            ┌───────────────┐   EVT_RX_TO/ERR     │
       │            │  WAIT RESP    ├──────────────────┐   │
       │            │  (1000 µs     │                  │   │
       │            │   timeout)    │                  │   │
       │            └───────┬───────┘                  │   │
       │                    │ EVT_RX_OK                │   │
       │                    │ + valid RESP frame        │   │
       │                    ▼                          │   │
       │            ┌───────────────┐   DWT_ERROR      │   │
       │            │  SEND FINAL   ├────────────┐     │   │
       │            │  (delayed TX, │            │     │   │
       │            │   sched_lock, │            │     │   │
       │            │   3 timestamps│            │     │   │
       │            │   in payload) │            │     │   │
       │            └───────┬───────┘            │     │   │
       │                    │ EVT_TX_DONE        │     │   │
       │                    ▼                    ▼     ▼   │
       │            ┌───────────────┐    ┌────────────┐    │
       │            │  CYCLE DONE   │    │   SLEEP    │    │
       │            │  range_count++│    │ interval_ms│    │
       │            └───────┬───────┘    └─────┬──────┘    │
       │                    │                  │           │
       │                    └──────┬───────────┘           │
       │                           │                       │
       │                    k_sleep(ranging_interval_ms)    │
       │                           │                       │
       └───────────────────────────┘                       │
                                                           │
              (tag also sends CoAP events at each step ────┘
               via thread_coap_send_event())
```

---

## Thread / CoAP Connectivity States

```
                    ┌──────────────┐
                    │   DETACHED   │
                    │  (booting)   │
                    └──────┬───────┘
                           │ openthread_run()
                           ▼
                    ┌──────────────┐
                    │  DISCOVERING │
                    │  (scanning   │
                    │   ch 15)     │
                    └──────┬───────┘
                           │ OT_CHANGED_THREAD_ROLE
                           ▼
              ┌────────────────────────┐
              │      CONNECTED         │
              │  (Child/Router/Leader) │
              │                        │
              │  thread_connected=true │
              │  CoAP sends enabled    │
              └────────────┬───────────┘
                           │ role → Disabled/Detached
                           ▼
                    ┌──────────────┐
                    │ DISCONNECTED │
                    │              │
                    │ thread_connected=false
                    │ CoAP sends silently dropped
                    └──────────────┘
```

Thread role is monitored via `on_thread_state_changed()` callback. The `thread_connected` flag gates all CoAP sends — when disconnected, `thread_coap_send_distance()` and `thread_coap_send_event()` return immediately without queueing.

---

## CoAP Data Reporting

Distance measurements and tag events are sent asynchronously via a dedicated work queue to avoid blocking the UWB thread.

### Data Flow (Anchor → Server)

```
uwb_manager: distance_cb(anchor_id, tag_id, dist_m)
  │
  ▼
main.c: on_distance_measured()
  ├─ blink LED2 (20ms)
  └─ thread_coap_send_distance()
       ├─ check thread_connected → return if false
       ├─ k_msgq_put(&meas_queue)  [depth=8, drop oldest if full]
       └─ k_work_submit_to_queue(&coap_workq, &send_work)
            │
            ▼ (runs on coap_workq thread, prio 5)
       coap_send_work_handler()
            └─ while (k_msgq_get): coap_send_request(POST /distance, payload)
```

### Data Flow (Tag → Server)

```
uwb_manager: initiator_loop()
  │
  ├─ thread_coap_send_event(tag_addr, UWB_EVT_POLL_TX, seq)
  ├─ thread_coap_send_event(tag_addr, UWB_EVT_RESP_RX, seq)    (on success)
  ├─ thread_coap_send_event(tag_addr, UWB_EVT_NO_RESP, seq)    (on timeout)
  └─ thread_coap_send_event(tag_addr, UWB_EVT_FINAL_TX, seq)   (on success)
       │
       └─ k_msgq_put(&evt_queue)  [depth=16]
          k_work_submit_to_queue(&coap_workq, &send_evt_work)
               │
               ▼ (runs on coap_workq thread)
          coap_send_evt_work_handler()
               └─ while (k_msgq_get): coap_send_request(POST /event, payload)
```

### Payload Formats

**POST /distance** — 20 bytes, little-endian (12-byte legacy also accepted by server):

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 2 | `anchor_id` | Anchor UWB short address |
| 2 | 2 | `tag_id` | Tag UWB short address |
| 4 | 4 | `distance_mm` | Distance in millimeters |
| 8 | 4 | `uptime_s` | Seconds since boot |
| 12 | 2 | `rssi_q8` | Channel RSSI in Q8.8 dBm (int16, divide by 256) |
| 14 | 2 | `fp_power_q8` | First-path power in Q8.8 dBm (int16, divide by 256) |
| 16 | 2 | `fp_index` | First-path CIR index, Q10.6 (uint16, divide by 64) |
| 18 | 2 | `peak_index` | Peak CIR sample index (uint16) |

**POST /event** — 6 bytes, little-endian:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 2 | `node_id` | Reporting node UWB short address |
| 2 | 1 | `event` | `0x01`=POLL_TX, `0x02`=RESP_RX, `0x03`=FINAL_TX, `0x10`=NO_RESP |
| 3 | 1 | `seq` | Frame sequence number |
| 4 | 2 | `reserved` | Zero |

### Signal Quality Diagnostics

The four diagnostic fields in the `/distance` payload are read from the DW3000's CIA (Channel Impulse Analysis) engine after each FINAL frame reception. They enable the server to assess measurement reliability and detect non-line-of-sight (NLOS) conditions.

#### RSSI (Received Signal Strength Indicator)

Total received channel power in dBm. Computed from the CIR (Channel Impulse Response) energy estimate using `dwt_calculate_rssi()`. Includes energy from all multipath components — the direct path plus any reflections.

Transmitted as Q8.8 fixed-point int16: divide by 256 to get dBm. Typical indoor UWB values range from -75 dBm (close range, clear LOS) to -95 dBm (far range or obstructed). More negative = weaker signal.

Useful for: link budget monitoring, coarse range estimation, environment characterization.

#### First Path Power

Signal power of only the direct (first-arriving) path component, in dBm (Q8.8 int16). The DW3000 identifies the first path arrival in the CIR and computes power from three amplitude samples (F1, F2, F3) around it using `dwt_calculate_first_path_power()`.

Always less than or equal to RSSI since it excludes multipath energy. When `RSSI - First Path Power > 6 dB`, significant multipath exists — the signal is arriving via reflections as well as the direct path. This is a strong indicator of NLOS or cluttered environments, and the distance measurement may be biased.

#### First Path Index

CIR sample index where the direct path was detected, in Q10.6 fixed-point (uint16, divide by 64 for the sample number). Read from `dwt_cirdiags_t.FpIndex`. Represents the arrival time of the earliest signal component, determined by the DW3000's leading-edge detection algorithm on the CIR.

Useful for: comparing with peak index to detect NLOS conditions.

#### Peak Path Index

CIR sample index of the strongest signal component (uint16, integer). Read from `dwt_cirdiags_t.peakIndex`.

In line-of-sight (LOS) conditions, the strongest path is the direct path, so `peak_index ≈ fp_index / 64`. In NLOS conditions, the direct path is attenuated by an obstruction while a reflected path (arriving later) is stronger, causing `peak_index > fp_index / 64`.

#### NLOS Detection Rules of Thumb

1. **`RSSI - FP Power > 6 dB`** — likely NLOS (multipath dominant over direct path)
2. **`Peak Index - FP Index/64 > 5 samples`** — likely NLOS (direct path attenuated, reflection stronger)
3. **Both conditions together** — high confidence NLOS
4. Distance measurements during NLOS are **biased long** (the reflected path is longer than the true direct-path distance)

---

## UCI Command Interface

Binary request/response protocol for device configuration. Transport-agnostic core (`uci.c`) with two transport layers.

### Frame Format

**Request** (host → device):
```
[0xAA] [CMD:1B] [LEN:1B] [PAYLOAD:0..64B] [CRC8:1B]
```

**Response** (device → host):
```
[0xBB] [CMD:1B] [STATUS:1B] [LEN:1B] [PAYLOAD:0..64B] [CRC8:1B]
```

CRC-8: polynomial `0x07`, init `0x00`, computed over all bytes before the CRC field.

### Command Table

| Opcode | Name | Payload (request) | Payload (response) | Effect |
|--------|------|-------|----------|--------|
| `0x01` | GET_INFO | — | `[ver_maj, ver_min, role, addr_lo, addr_hi, running]` (6B) | Read firmware info |
| `0x02` | SET_ROLE | `[role]` (1B: 0=anchor, 1=tag) | — | Set role (save+reboot to apply) |
| `0x03` | SET_ADDR | `[addr_lo, addr_hi]` (2B LE) | — | Set UWB short address |
| `0x04` | SET_INTERVAL | `[ms_lo, ms_hi]` (2B LE, 50–10000) | — | Set ranging interval (immediate) |
| `0x05` | SET_SERVER | `[ipv6_string\0, port_lo, port_hi]` | — | Set CoAP server address |
| `0x10` | START | — | — | Start UWB ranging |
| `0x11` | STOP | — | — | Stop UWB ranging |
| `0x12` | GET_STATUS | — | `[running, dist_mm(4B), uptime_s(4B), count(4B)]` (13B) | Read runtime status |
| `0x20` | SAVE_CONFIG | — | — | Persist g_config to NVS |
| `0x21` | FACTORY_RESET | — | — | Erase NVS, reboot |
| `0x22` | ENTER_BOOTLOADER | — | — | Reboot into MCUboot serial recovery |

### Status Codes

| Code | Name | Meaning |
|------|------|---------|
| `0x00` | OK | Success |
| `0x01` | ERR_UNKNOWN_CMD | Unrecognized opcode |
| `0x02` | ERR_BAD_PAYLOAD | Wrong payload length |
| `0x03` | ERR_INVALID_VAL | Value out of range |
| `0x04` | ERR_BUSY | Already running / save failed |

### Transport: UART (`uci_uart.c`)

```
UART ISR → rx_ring[128] → rx_sem
                              │
                     uci_uart thread (prio 8)
                              │
                     State Machine:
                       WAIT_SYNC → READ_CMD → READ_LEN → READ_PAYLOAD → READ_CRC
                              │
                     CRC valid? → uci_process_locked() → serialize → uart_poll_out()
```

States:
- **WAIT_SYNC**: discard bytes until `0xAA` received
- **READ_CMD**: store command byte
- **READ_LEN**: store payload length; if 0, skip to READ_CRC
- **READ_PAYLOAD**: collect `LEN` payload bytes
- **READ_CRC**: validate CRC-8, dispatch if valid, reset to WAIT_SYNC

Full UART framing (sync + CRC) is used because UART has no built-in framing.

### Transport: CoAP (`uci_coap.c`)

Registers `POST /cmd` on the OpenThread CoAP server (port 5683). No sync byte or CRC — CoAP/UDP provides framing and integrity.

Request payload: `[CMD:1B] [LEN:1B] [PAYLOAD:0..64B]`
Response payload: `[CMD:1B] [STATUS:1B] [LEN:1B] [PAYLOAD:0..64B]`

Responses use `2.04 Changed` for success, `4.00 Bad Request` for errors.

### Thread Safety

Both transports call `uci_process_locked()`, which acquires `uci_lock` (a `k_mutex`) before dispatching to `uci_process()`. This prevents concurrent UART and CoAP commands from corrupting state.

---

## Configuration System

### Layering

```
┌──────────────────────────┐
│  Runtime g_config struct │  ← what all code reads
├──────────────────────────┤
│  NVS flash (settings)   │  ← overrides Kconfig on boot
├──────────────────────────┤
│  Kconfig defaults        │  ← compiled-in fallbacks
└──────────────────────────┘
```

1. `load_defaults()` copies Kconfig values into `g_config`
2. `settings_load()` calls `config_set()` for each key found in NVS, overwriting the defaults
3. Runtime changes (via UCI SET commands) modify `g_config` in memory only
4. `device_config_save()` (UCI SAVE_CONFIG) persists all fields to NVS under the `uwb/` namespace
5. `device_config_reset()` (UCI FACTORY_RESET) deletes all NVS keys and reloads defaults

### NVS Keys

| Key | Type | g_config field |
|-----|------|----------------|
| `uwb/role` | uint8 | `role` |
| `uwb/addr` | uint16 | `uwb_addr` |
| `uwb/interval` | uint16 | `ranging_interval_ms` |
| `uwb/server` | string | `server_addr` |
| `uwb/port` | uint16 | `server_port` |
| `uwb/autostart` | bool | `autostart` |

### Kconfig Defaults

| Kconfig Symbol | Default | Description |
|----------------|---------|-------------|
| `NODE_ROLE_ANCHOR` | y (anchor board conf) | Anchor role |
| `NODE_ROLE_TAG` | y (tag board conf) | Tag role |
| `UWB_NODE_SHORT_ADDR` | `0x0001` (anchor), `0x0100` (tag) | UWB address |
| `UWB_RANGING_INTERVAL_MS` | 1000 | Ranging interval |
| `COAP_SERVER_ADDR` | `"ff03::1"` | Multicast default |
| `COAP_SERVER_PORT` | 5683 | CoAP port |
| `UCI_AUTOSTART` | y | Auto-start ranging |

---

## Stop / Start Behavior

Stopping and restarting ranging requires careful cleanup to avoid stale DW3000 state:

```
uwb_manager_stop()
  └─ atomic_set(&uwb_running, 0)
       │
       ▼ (next loop iteration in responder_loop/initiator_loop)
       dwt_forcetrxoff()           ← abort any pending TX/RX
       was_stopped = true
       k_sleep(100ms) loop         ← idle polling

uwb_manager_start()
  └─ atomic_set(&uwb_running, 1)
       │
       ▼ (detected on next loop iteration)
       drain_semaphores()          ← clear stale TX/RX/TO/ERR events
       dwt_forcetrxoff()           ← ensure clean radio state
       dwt_writesysstatuslo(...)   ← clear all pending status bits
       was_stopped = false
       resume normal operation
```

The UWB thread is created once and never terminated. The `uwb_started` flag ensures `k_thread_create()` is only called on the first start.

---

## OTA Update Architecture

### Anchor (nRF52840 DK) — Dual-Slot Swap

MCUmgr SMP over UDP (port 1337) with MCUboot bootloader. Upload → stage in slot 1 → reboot → MCUboot swaps slots → confirm or rollback.

```
┌─────────────┐    ┌─────────────┐
│  Slot 0     │    │  Slot 1     │
│ (active app)│    │ (staging)   │
└──────┬──────┘    └──────┬──────┘
       │                  │
       │   mcumgr upload ─┘
       │                  │
       │◄── MCUboot swap ─┘
       │    on reboot
```

### Tag (DWM3001CDK) — Single-Slot Serial Recovery

MCUboot in single-slot mode. Serial recovery via UART: hold P0.02 button during reset, then upload via `mcumgr --conntype serial`.

### Enter Bootloader (UCI Command 0x22)

Available on boards with `CONFIG_RETENTION_BOOT_MODE=y`. Sets MCUboot boot mode flag via retention memory, then reboots. MCUboot detects the flag and enters serial recovery mode instead of jumping to the app.

---

## System-Level State Diagram

```
                         ┌─────────┐
                         │  RESET  │
                         └────┬────┘
                              │
                         MCUboot runs
                         (verify slot 0)
                              │
                              ▼
                       ┌────────────┐
                       │    BOOT    │
                       │            │
                       │ config     │
                       │ thread     │
                       │ coap       │
                       │ uwb init   │
                       │ uci init   │
                       └──────┬─────┘
                              │
                     ┌────────┴────────┐
                     │                 │
              autostart=true    autostart=false
                     │                 │
                     ▼                 ▼
              ┌────────────┐   ┌────────────┐
              │  RANGING   │   │   READY    │
              │            │   │ (idle,     │
              │ DS-TWR     │   │  awaiting  │
              │ active     │   │  UCI START)│
              │            │   │            │
              └──┬───┬─────┘   └─────┬──────┘
                 │   │               │
           UCI   │   │ UCI      UCI  │
           STOP  │   │ FACTORY  START│
                 │   │ RESET         │
                 ▼   │               ▼
          ┌──────────┤        ┌────────────┐
          │  READY   │        │  RANGING   │
          └──────────┘        └────────────┘
                 │
                 │ UCI ENTER_BOOTLOADER
                 ▼
          ┌──────────────┐
          │  BOOTLOADER  │
          │  (MCUboot    │
          │   serial     │
          │   recovery)  │
          └──────────────┘
```

Any state can transition to RESET via UCI FACTORY_RESET (erases NVS, cold reboot) or hardware reset.

---

## IPv6/CoAP Server Integration Guide

This section is for developers building a server application that communicates with the UWB devices over the Thread IPv6 network. No firmware source access is needed — only a connection to the Thread mesh.

### Network Prerequisites

The server host must be on the same Thread network as the devices. Required parameters (must match exactly):

| Parameter | Value | Notes |
|-----------|-------|-------|
| Channel | 15 | IEEE 802.15.4 |
| PAN ID | `0xABCD` (43981) | |
| Extended PAN ID | `1111111122222222` | |
| Network Key | `00112233445566778899aabbccddeeff` | AES-128, colon-separated in firmware config |
| Network Name | `ot_zephyr` | |
| Mesh-Local Prefix | `fdde:ad00:beef::/64` | Devices get addresses under this prefix |

**Host setup**: Run `scripts/thread_dongle_setup.sh` on the Linux host with an nRF52840 USB dongle as Thread RCP. This starts `ot-daemon`, configures the dataset, brings up `wpan0`, and joins the multicast group.

**Multicast group**: Devices send data to `ff03::1` (Thread realm-local all-nodes). The server host must join this group:

```bash
sudo ip -6 maddr add ff03::1 dev wpan0
```

**Discovering device addresses**:

```bash
sudo ot-ctl neighbor table    # list mesh neighbors
ping6 -c3 -I wpan0 ff03::1   # multicast ping to find all devices
```

Devices receive mesh-local unicast addresses (e.g. `fdde:ad00:beef:0:a4b1:c2d3:e4f5:6789`). Use these for sending commands to specific devices.

---

### Receiving Data from Devices

Devices POST binary payloads to the server. Your CoAP server must listen on port **5683** and register these two resources:

#### `POST /distance` — Distance Measurements (sent by anchors)

20-byte binary payload, little-endian (12-byte legacy format also accepted for backwards compatibility):

```
Offset  Size  Type      Field         Description
──────  ────  ────────  ───────────   ─────────────────────────────────
0       2     uint16    anchor_id     Anchor UWB short address (e.g. 0x0001)
2       2     uint16    tag_id        Tag UWB short address (e.g. 0x0100)
4       4     uint32    distance_mm   Measured distance in millimeters
8       4     uint32    uptime_s      Seconds since anchor booted
12      2     int16     rssi_q8       Channel RSSI, Q8.8 dBm (divide by 256)
14      2     int16     fp_power_q8   First-path power, Q8.8 dBm (divide by 256)
16      2     uint16    fp_index      First-path CIR index, Q10.6 (divide by 64)
18      2     uint16    peak_index    Peak CIR sample index
```

Python parsing (backwards-compatible):

```python
import struct

DIST_V2 = struct.Struct("<HHIIhhHH")  # 20 bytes
DIST_V1 = struct.Struct("<HHII")       # 12 bytes (legacy)

def parse_distance(payload: bytes) -> dict:
    if len(payload) == DIST_V2.size:
        (anchor_id, tag_id, distance_mm, uptime_s,
         rssi_q8, fp_power_q8, fp_idx, peak_idx) = DIST_V2.unpack(payload)
        return {
            "anchor_id": anchor_id,
            "tag_id": tag_id,
            "distance_m": distance_mm / 1000.0,
            "uptime_s": uptime_s,
            "rssi_dbm": rssi_q8 / 256.0,
            "fp_power_dbm": fp_power_q8 / 256.0,
            "fp_index": fp_idx / 64.0,
            "peak_index": peak_idx,
        }
    elif len(payload) == DIST_V1.size:
        anchor_id, tag_id, distance_mm, uptime_s = DIST_V1.unpack(payload)
        return {
            "anchor_id": anchor_id,
            "tag_id": tag_id,
            "distance_m": distance_mm / 1000.0,
            "uptime_s": uptime_s,
        }
```

Rate: one POST per ranging cycle (default interval: 1000 ms, configurable 50–10000 ms). Only sent when ranging is active and Thread is connected.

#### `POST /event` — Tag UWB Events (sent by tags)

6-byte binary payload, little-endian:

```
Offset  Size  Type      Field       Description
──────  ────  ────────  ─────────   ──────────────────────────────────
0       2     uint16    node_id     Tag UWB short address
2       1     uint8     event       Event type (see table below)
3       1     uint8     seq         Frame sequence number
4       2     uint16    reserved    Always zero
```

Event types:

| Code | Name | Meaning |
|------|------|---------|
| `0x01` | `POLL_TX` | Tag sent a POLL frame (ranging cycle started) |
| `0x02` | `RESP_RX` | Tag received RESP from anchor |
| `0x03` | `FINAL_TX` | Tag sent FINAL frame (ranging cycle complete) |
| `0x10` | `NO_RESP` | Anchor did not respond (timeout) |

Python parsing:

```python
EVT_FMT = struct.Struct("<HBBxx")  # 6 bytes

def parse_event(payload: bytes) -> dict:
    node_id, event, seq = EVT_FMT.unpack(payload)
    return {"node_id": node_id, "event": event, "seq": seq}
```

Tags send multiple events per ranging cycle (POLL_TX, then RESP_RX or NO_RESP, then FINAL_TX on success). The event queue depth is 16.

#### Server Implementation

Minimal aiocoap server:

```python
import aiocoap
import aiocoap.resource as resource

class DistanceResource(resource.Resource):
    async def render_post(self, request):
        if len(request.payload) in (DIST_V2.size, DIST_V1.size):
            data = parse_distance(request.payload)
            sender = request.remote.hostinfo  # device IPv6 address
            # ... store data ...
        return aiocoap.Message(code=aiocoap.CHANGED)

class EventResource(resource.Resource):
    async def render_post(self, request):
        if len(request.payload) == 6:
            data = parse_event(request.payload)
            # ... handle event ...
        return aiocoap.Message(code=aiocoap.CHANGED)

site = resource.Site()
site.add_resource(["distance"], DistanceResource())
site.add_resource(["event"], EventResource())
context = await aiocoap.Context.create_server_context(site, bind=("::", 5683))
```

Bind to `::` (all IPv6 interfaces) to receive traffic on `wpan0`. Devices identify themselves via the IPv6 source address (`request.remote.hostinfo`) and UWB short address in the payload.

Default device configuration sends to multicast `ff03::1:5683`. To redirect a device to a specific server address, use the UCI SET_SERVER command (see below).

---

### Sending Commands to Devices (Remote UCI over CoAP)

Each device runs a CoAP server on port **5683** with a `POST /cmd` resource. Commands are sent as binary payloads — no sync byte or CRC (CoAP provides framing and integrity).

#### Request Format

```
[CMD:1B] [LEN:1B] [PAYLOAD:0..64B]
```

#### Response Format

```
[CMD:1B] [STATUS:1B] [LEN:1B] [PAYLOAD:0..64B]
```

CoAP response codes: `2.04 Changed` on success, `4.00 Bad Request` on error.

#### Status Codes

| Code | Name | Meaning |
|------|------|---------|
| `0x00` | OK | Success |
| `0x01` | ERR_UNKNOWN_CMD | Unrecognized command opcode |
| `0x02` | ERR_BAD_PAYLOAD | Wrong payload length |
| `0x03` | ERR_INVALID_VAL | Value out of valid range |
| `0x04` | ERR_BUSY | Already running / operation failed |

#### Command Reference

**GET_INFO** (`0x01`) — Query device identity

```
Request:  [0x01] [0x00]
Response: [0x01] [status] [0x06] [fw_major] [fw_minor] [role] [addr_lo] [addr_hi] [running]
```

- `role`: `0x00` = anchor, `0x01` = tag
- `addr`: UWB short address (little-endian uint16)
- `running`: `0x01` if ranging is active

**GET_STATUS** (`0x12`) — Query runtime status

```
Request:  [0x12] [0x00]
Response: [0x12] [status] [0x0D] [running:1B] [last_dist_mm:4B LE] [uptime_s:4B LE] [range_count:4B LE]
```

- `last_dist_mm`: last measured distance in millimeters (uint32 LE)
- `uptime_s`: seconds since boot (uint32 LE)
- `range_count`: total successful ranging cycles (uint32 LE)

**START** (`0x10`) — Start UWB ranging

```
Request:  [0x10] [0x00]
Response: [0x10] [status] [0x00]
```

Returns `ERR_BUSY` if already running.

**STOP** (`0x11`) — Stop UWB ranging

```
Request:  [0x11] [0x00]
Response: [0x11] [status] [0x00]
```

**SET_ROLE** (`0x02`) — Change device role

```
Request:  [0x02] [0x01] [role]
Response: [0x02] [status] [0x00]
```

- `role`: `0x00` = anchor, `0x01` = tag
- Requires SAVE_CONFIG + reboot to take effect

**SET_ADDR** (`0x03`) — Change UWB short address

```
Request:  [0x03] [0x02] [addr_lo] [addr_hi]
Response: [0x03] [status] [0x00]
```

- Address range: `0x0001`–`0xFFFE` (little-endian uint16)

**SET_INTERVAL** (`0x04`) — Change ranging interval

```
Request:  [0x04] [0x02] [ms_lo] [ms_hi]
Response: [0x04] [status] [0x00]
```

- Range: 50–10000 ms (little-endian uint16)
- Takes effect immediately (no save/reboot needed)

**SET_SERVER** (`0x05`) — Change CoAP reporting destination

```
Request:  [0x05] [LEN] [ipv6_string_with_null] [port_lo] [port_hi]
Response: [0x05] [status] [0x00]
```

- `ipv6_string_with_null`: null-terminated ASCII IPv6 address (e.g. `ff03::1\0`)
- `port`: little-endian uint16

Example — set server to `fdde:ad00:beef::1` port 5683:

```
[0x05] [0x14] [66 64 64 65 3a 61 64 30 30 3a 62 65 65 66 3a 3a 31 00] [83 16]
        LEN=20  "fdde:ad00:beef::1\0" (18 bytes)                        5683 LE
```

**SAVE_CONFIG** (`0x20`) — Persist settings to flash

```
Request:  [0x20] [0x00]
Response: [0x20] [status] [0x00]
```

Settings survive reboot only after SAVE_CONFIG is called. Without it, changes are lost on power cycle.

**FACTORY_RESET** (`0x21`) — Erase settings, reboot

```
Request:  [0x21] [0x00]
Response: [0x21] [0x00] [0x00]   (sent before reboot — may not arrive)
```

Device erases all NVS settings and cold reboots. Connection will be lost.

**ENTER_BOOTLOADER** (`0x22`) — Enter MCUboot serial recovery

```
Request:  [0x22] [0x00]
Response: [0x22] [status] [0x00]   (sent before reboot — may not arrive)
```

Device reboots into MCUboot serial recovery mode. Only supported on boards with retention boot mode. Connection will be lost.

#### Python Client Example

```python
import asyncio
import struct
import aiocoap

async def send_uci_command(device_ipv6: str, cmd: int, payload: bytes = b"") -> tuple:
    """Send a UCI command to a device over CoAP. Returns (status, payload)."""
    uri = f"coap://[{device_ipv6}]:5683/cmd"
    coap_payload = bytes([cmd, len(payload)]) + payload

    request = aiocoap.Message(code=aiocoap.POST, uri=uri, payload=coap_payload)
    context = await aiocoap.Context.create_client_context()
    try:
        response = await asyncio.wait_for(
            context.request(request).response, timeout=5.0
        )
    finally:
        await context.shutdown()

    rsp = response.payload
    rsp_status = rsp[1]
    rsp_len = rsp[2]
    rsp_payload = rsp[3:3 + rsp_len] if rsp_len > 0 else b""
    return rsp_status, rsp_payload

# Example: get device info
status, data = asyncio.run(send_uci_command("fdde:ad00:beef:0:1234:5678:9abc:def0", 0x01))

# Example: set ranging interval to 500 ms
status, _ = asyncio.run(send_uci_command(
    "fdde:ad00:beef:0:1234:5678:9abc:def0",
    0x04,
    struct.pack("<H", 500),
))

# Example: start ranging
status, _ = asyncio.run(send_uci_command("fdde:ad00:beef:0:1234:5678:9abc:def0", 0x10))
```

---

### OTA Firmware Updates over IPv6

Anchors support over-the-air updates via **MCUmgr SMP over UDP** on port **1337**. Tags use serial recovery only (not OTA).

#### Anchor OTA Workflow

```bash
# 1. Upload new signed image to slot 1
mcumgr --conntype udp --connstring="[<device-ipv6>]:1337" image upload zephyr.signed.bin

# 2. Device reboots into new image (test mode)
mcumgr --conntype udp --connstring="[<device-ipv6>]:1337" reset

# 3. Confirm the new image (prevent rollback)
mcumgr --conntype udp --connstring="[<device-ipv6>]:1337" image confirm

# 4. List installed images
mcumgr --conntype udp --connstring="[<device-ipv6>]:1337" image list
```

Upload speed is ~6.9 KiB/s over Thread (~49 seconds for a typical image). The device runs dual-slot MCUboot with swap — if the new image is not confirmed, it rolls back to the previous version on next reboot.

Install the mcumgr CLI: `go install github.com/apache/mynewt-mcumgr-cli/mcumgr@latest`

---

### Address Conventions

| Address Range | Role | Example |
|---------------|------|---------|
| `0x0001`–`0x00FF` | Anchors | `0x0001` = first anchor |
| `0x0100`–`0xFFFE` | Tags | `0x0100` = first tag |
| `0x0000`, `0xFFFF` | Reserved | Cannot be assigned |

---

### Quick-Start Checklist

1. Set up Thread border router on your host (`scripts/thread_dongle_setup.sh`)
2. Verify connectivity: `ping6 -c3 -I wpan0 ff03::1`
3. Discover device IPv6 addresses: `sudo ot-ctl neighbor table`
4. Start your CoAP server on `[::]:5683` with `/distance` and `/event` POST resources
5. Join multicast group: `sudo ip -6 maddr add ff03::1 dev wpan0`
6. Verify data flows: devices auto-start ranging on boot and POST to `ff03::1:5683`
7. Send commands to a specific device: `POST coap://[device-ipv6]:5683/cmd`
8. To redirect a device to your unicast address: send SET_SERVER + SAVE_CONFIG
