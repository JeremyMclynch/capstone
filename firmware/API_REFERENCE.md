# API & SDK Reference

Every non-standard-C function, type, macro, and constant used by the firmware, organized by library. Includes on-disk paths for each SDK/module.

---

## Build Dependency Tree

```
firmware/CMakeLists.txt
│
├── ZEPHYR_EXTRA_MODULES
│   └── ../zephyr-dw3000-decadriver/           ← DW3000 UWB driver
│       ├── dwt_uwb_driver/                       (core driver API)
│       └── platform/                              (Zephyr HAL layer)
│
└── find_package(Zephyr)  →  ~/ncs/v3.2.2/
    │
    ├── zephyr/                                 ← Zephyr RTOS
    │   ├── include/zephyr/                        (kernel, drivers, net, settings, ...)
    │   └── modules/openthread/                    (Zephyr ↔ OpenThread glue)
    │
    ├── modules/lib/openthread/                 ← OpenThread stack
    │   └── include/openthread/                    (thread.h, coap.h, message.h)
    │
    └── nrf/                                    ← nRF SDK extensions
        ├── include/dk_buttons_and_leds.h          (LED/button helpers)
        └── include/net/coap_utils.h               (CoAP send helper)
```

---

## 1. DW3000 Zephyr Driver

**Disk path:** `/home/jeremy/Projects/capstone/zephyr-dw3000-decadriver/`
**Referenced via:** `ZEPHYR_EXTRA_MODULES` in `firmware/CMakeLists.txt`
**Kconfig:** `CONFIG_DW3000=y`, `CONFIG_DW3000_CHIP_DW3000=y`

### Key Headers

| Header | Path (relative to module root) | Purpose |
|--------|-------------------------------|---------|
| `deca_device_api.h` | `dwt_uwb_driver/deca_device_api.h` | Core transceiver API |
| `deca_probe_interface.h` | `platform/deca_probe_interface.h` | SPI probe/detect |
| `dw3000_hw.h` | `platform/dw3000_hw.h` | GPIO, reset, IRQ init |
| `dw3000_spi.h` | `platform/dw3000_spi.h` | SPI speed control |
| `deca_types.h` | `dwt_uwb_driver/deca_types.h` | Type definitions |
| `deca_interface.h` | `dwt_uwb_driver/deca_interface.h` | Driver interface structs |
| `dw3000_deca_regs.h` | `dwt_uwb_driver/dw3000/dw3000_deca_regs.h` | DW3000 register map |
| `dw3720_deca_regs.h` | `dwt_uwb_driver/dw3720/dw3720_deca_regs.h` | DW3720 register map (tag) |

### Functions

| Function | Used in | Header | Description |
|----------|---------|--------|-------------|
| `dw3000_hw_init()` | `uwb_manager.c` | `dw3000_hw.h` | Initialize GPIO and SPI pins |
| `dw3000_hw_reset()` | `uwb_manager.c` | `dw3000_hw.h` | Hardware reset pulse |
| `dw3000_hw_init_interrupt()` | `uwb_manager.c` | `dw3000_hw.h` | Connect GPIO IRQ to dwt_isr |
| `dw3000_spi_speed_fast()` | `uwb_manager.c` | `dw3000_spi.h` | Switch SPI from 2 MHz to fast clock |
| `dwt_probe()` | `uwb_manager.c` | `deca_probe_interface.h` | Detect DW3000 on SPI bus |
| `dwt_initialise()` | `uwb_manager.c` | `deca_device_api.h` | Chip init, load OTP trim values |
| `dwt_checkidlerc()` | `uwb_manager.c` | `deca_device_api.h` | Poll until chip enters idle state |
| `dwt_configure()` | `uwb_manager.c` | `deca_device_api.h` | Set PHY config (channel, rate, preamble) |
| `dwt_configuretxrf()` | `uwb_manager.c` | `deca_device_api.h` | Set TX power configuration |
| `dwt_otpread()` | `uwb_manager.c` | `deca_device_api.h` | Read one-time-programmable memory |
| `dwt_setrxantennadelay()` | `uwb_manager.c` | `deca_device_api.h` | Set RX antenna delay calibration |
| `dwt_settxantennadelay()` | `uwb_manager.c` | `deca_device_api.h` | Set TX antenna delay calibration |
| `dwt_setlnapamode()` | `uwb_manager.c` | `deca_device_api.h` | Enable LNA + PA |
| `dwt_setleds()` | `uwb_manager.c` | `deca_device_api.h` | Enable on-chip TX/RX indicator LEDs |
| `dwt_setcallbacks()` | `uwb_manager.c` | `deca_device_api.h` | Register TX/RX/timeout/error callbacks |
| `dwt_setinterrupt()` | `uwb_manager.c` | `deca_device_api.h` | Configure IRQ mask |
| `dwt_rxenable()` | `uwb_manager.c` | `deca_device_api.h` | Enable receiver |
| `dwt_starttx()` | `uwb_manager.c` | `deca_device_api.h` | Start transmission (immediate or delayed) |
| `dwt_forcetrxoff()` | `uwb_manager.c` | `deca_device_api.h` | Force transceiver off (abort TX/RX) |
| `dwt_writetxdata()` | `uwb_manager.c` | `deca_device_api.h` | Write frame data to TX buffer |
| `dwt_writetxfctrl()` | `uwb_manager.c` | `deca_device_api.h` | Set TX frame control (length, ranging flag) |
| `dwt_readrxdata()` | `uwb_manager.c` | `deca_device_api.h` | Read received frame data |
| `dwt_getframelength()` | `uwb_manager.c` | `deca_device_api.h` | Get length of received frame |
| `dwt_readtxtimestamp()` | `uwb_manager.c` | `deca_device_api.h` | Read TX timestamp (5 bytes) |
| `dwt_readrxtimestamp()` | `uwb_manager.c` | `deca_device_api.h` | Read RX timestamp (5 bytes, needs `DWT_IP_M`) |
| `dwt_setdelayedtrxtime()` | `uwb_manager.c` | `deca_device_api.h` | Set delayed TX/RX time |
| `dwt_setrxaftertxdelay()` | `uwb_manager.c` | `deca_device_api.h` | Set delay before auto-RX after TX |
| `dwt_setrxtimeout()` | `uwb_manager.c` | `deca_device_api.h` | Set RX frame timeout |
| `dwt_setpreambledetecttimeout()` | `uwb_manager.c` | `deca_device_api.h` | Set preamble detection timeout |
| `dwt_writesysstatuslo()` | `uwb_manager.c` | `deca_device_api.h` | Clear system status register bits |

### Constants

| Constant | Header | Description |
|----------|--------|-------------|
| `DWT_DW_INIT` | `deca_device_api.h` | Init mode flag for `dwt_initialise()` |
| `DWT_SUCCESS` | `deca_device_api.h` | Return code: success |
| `DWT_ERROR` | `deca_device_api.h` | Return code: error |
| `DWT_PLEN_128` | `deca_device_api.h` | Preamble length: 128 symbols |
| `DWT_PAC8` | `deca_device_api.h` | Preamble acquisition chunk: 8 |
| `DWT_BR_6M8` | `deca_device_api.h` | Bit rate: 6.8 Mbps |
| `DWT_PHRMODE_STD` | `deca_device_api.h` | Standard PHR mode |
| `DWT_PHRRATE_STD` | `deca_device_api.h` | Standard PHR rate |
| `DWT_STS_MODE_OFF` | `deca_device_api.h` | STS disabled |
| `DWT_STS_LEN_64` | `deca_device_api.h` | STS length (unused when off) |
| `DWT_PDOA_M0` | `deca_device_api.h` | PDoA mode 0 (off) |
| `DWT_IP_M` | `deca_device_api.h` | RX timestamp read mode parameter |
| `DWT_TIME_UNITS` | `deca_device_api.h` | DW timestamp unit in seconds (~15.65 ps) |
| `FCS_LEN` | `deca_device_api.h` | Frame check sequence length (2 bytes) |
| `DWT_START_RX_IMMEDIATE` | `deca_device_api.h` | Start RX immediately |
| `DWT_START_TX_IMMEDIATE` | `deca_device_api.h` | Start TX immediately |
| `DWT_START_TX_DELAYED` | `deca_device_api.h` | Start TX at programmed time |
| `DWT_RESPONSE_EXPECTED` | `deca_device_api.h` | Auto-enable RX after TX |
| `DWT_LNA_ENABLE` | `deca_device_api.h` | Enable low-noise amplifier |
| `DWT_PA_ENABLE` | `deca_device_api.h` | Enable power amplifier |
| `DWT_LEDS_ENABLE` | `deca_device_api.h` | Enable DW3000 LEDs |
| `DWT_LEDS_INIT_BLINK` | `deca_device_api.h` | Blink LEDs on init |
| `DWT_ENABLE_INT_ONLY` | `deca_device_api.h` | Interrupt config: enable only specified bits |
| `DWT_INT_TXFRS_BIT_MASK` | `deca_device_api.h` | IRQ: TX frame sent |
| `DWT_INT_RXFCG_BIT_MASK` | `deca_device_api.h` | IRQ: RX frame OK (FCS good) |
| `DWT_INT_RXFTO_BIT_MASK` | `deca_device_api.h` | IRQ: RX frame timeout |
| `DWT_INT_RXPTO_BIT_MASK` | `deca_device_api.h` | IRQ: RX preamble timeout |
| `DWT_INT_RXPHE_BIT_MASK` | `deca_device_api.h` | IRQ: RX PHR error |
| `DWT_INT_RXFCE_BIT_MASK` | `deca_device_api.h` | IRQ: RX FCS error |
| `DWT_INT_RXFSL_BIT_MASK` | `deca_device_api.h` | IRQ: RX frame sync loss |
| `DWT_INT_RXSTO_BIT_MASK` | `deca_device_api.h` | IRQ: RX SFD timeout |
| `SYS_STATUS_ALL_RX_TO` | `deca_device_api.h` | Bitmask: all RX timeout status bits |
| `SYS_STATUS_ALL_RX_ERR` | `deca_device_api.h` | Bitmask: all RX error status bits |

### Types

| Type | Header | Description |
|------|--------|-------------|
| `dwt_config_t` | `deca_device_api.h` | PHY configuration struct |
| `dwt_txconfig_t` | `deca_device_api.h` | TX power configuration struct |
| `dwt_cb_data_t` | `deca_device_api.h` | Callback data (passed to IRQ callbacks) |
| `dwt_callbacks_s` | `deca_device_api.h` | Callback function pointers struct |
| `dwt_probe_s` | `deca_probe_interface.h` | SPI probe interface struct |
| `dw3000_probe_interf` | `deca_probe_interface.h` | Global probe interface instance |

---

## 2. OpenThread

**Disk path (library):** `/home/jeremy/ncs/v3.2.2/modules/lib/openthread/`
**Disk path (Zephyr integration):** `/home/jeremy/ncs/v3.2.2/zephyr/modules/openthread/`
**Kconfig:** `CONFIG_OPENTHREAD=y`, `CONFIG_OPENTHREAD_FTD=y`, `CONFIG_OPENTHREAD_COAP=y`

### Key Headers

| Header | Disk Path | Purpose |
|--------|-----------|---------|
| `openthread/thread.h` | `modules/lib/openthread/include/openthread/thread.h` | Thread role and state API |
| `openthread/coap.h` | `modules/lib/openthread/include/openthread/coap.h` | CoAP resource/message API |
| `openthread/message.h` | `modules/lib/openthread/include/openthread/message.h` | Message buffer API |
| `openthread.h` | `zephyr/modules/openthread/include/openthread.h` | Zephyr ↔ OT glue (callbacks, run) |

All `modules/lib/openthread/` paths are relative to `~/ncs/v3.2.2/`.

### Functions — Thread State & Lifecycle

| Function | Used in | Header | Description |
|----------|---------|--------|-------------|
| `openthread_get_default_instance()` | `thread_coap.c`, `uci_coap.c` | `openthread.h` | Get the singleton OT instance |
| `openthread_state_changed_callback_register()` | `thread_coap.c` | `openthread.h` | Register Thread role change callback |
| `openthread_run()` | `thread_coap.c` | `openthread.h` | Start the OpenThread stack |
| `otThreadGetDeviceRole()` | `thread_coap.c` | `openthread/thread.h` | Query current Thread role |

### Functions — CoAP Server (UCI transport)

| Function | Used in | Header | Description |
|----------|---------|--------|-------------|
| `otCoapAddResource()` | `uci_coap.c` | `openthread/coap.h` | Register a CoAP resource |
| `otCoapStart()` | `uci_coap.c` | `openthread/coap.h` | Start OT CoAP server on port |
| `otCoapMessageGetCode()` | `uci_coap.c` | `openthread/coap.h` | Get CoAP method from request |
| `otCoapMessageGetType()` | `uci_coap.c` | `openthread/coap.h` | Get CoAP message type (CON/NON) |
| `otCoapNewMessage()` | `uci_coap.c` | `openthread/coap.h` | Allocate a new CoAP message |
| `otCoapMessageInitResponse()` | `uci_coap.c` | `openthread/coap.h` | Initialize a CoAP response |
| `otCoapMessageSetPayloadMarker()` | `uci_coap.c` | `openthread/coap.h` | Add payload marker (0xFF) |
| `otCoapSendResponse()` | `uci_coap.c` | `openthread/coap.h` | Send CoAP response |

### Functions — Message Buffer

| Function | Used in | Header | Description |
|----------|---------|--------|-------------|
| `otMessageGetOffset()` | `uci_coap.c` | `openthread/message.h` | Get payload offset in message |
| `otMessageGetLength()` | `uci_coap.c` | `openthread/message.h` | Get total message length |
| `otMessageRead()` | `uci_coap.c` | `openthread/message.h` | Read bytes from message buffer |
| `otMessageAppend()` | `uci_coap.c` | `openthread/message.h` | Append bytes to message buffer |
| `otMessageFree()` | `uci_coap.c` | `openthread/message.h` | Free a message buffer |

### Constants & Types

| Symbol | Header | Description |
|--------|--------|-------------|
| `OT_DEVICE_ROLE_CHILD` | `openthread/thread.h` | Thread role: child |
| `OT_DEVICE_ROLE_ROUTER` | `openthread/thread.h` | Thread role: router |
| `OT_DEVICE_ROLE_LEADER` | `openthread/thread.h` | Thread role: leader |
| `OT_CHANGED_THREAD_ROLE` | `openthread/thread.h` | State-change flag: role changed |
| `OT_COAP_TYPE_CONFIRMABLE` | `openthread/coap.h` | CON message type |
| `OT_COAP_TYPE_ACKNOWLEDGMENT` | `openthread/coap.h` | ACK message type |
| `OT_COAP_TYPE_NON_CONFIRMABLE` | `openthread/coap.h` | NON message type |
| `OT_COAP_CODE_POST` | `openthread/coap.h` | CoAP POST method code |
| `OT_COAP_CODE_CHANGED` | `openthread/coap.h` | CoAP 2.04 Changed response |
| `OT_COAP_CODE_BAD_REQUEST` | `openthread/coap.h` | CoAP 4.00 Bad Request response |
| `OT_ERROR_NONE` | `openthread/error.h` | Success return code |
| `otInstance` | `openthread/instance.h` | Opaque OT instance type |
| `otMessage` | `openthread/message.h` | Message buffer type |
| `otMessageInfo` | `openthread/message.h` | Message addressing info |
| `otCoapResource` | `openthread/coap.h` | CoAP resource definition |
| `otDeviceRole` | `openthread/thread.h` | Thread role enum |
| `otChangedFlags` | `openthread/instance.h` | State-change bitmask type |
| `otError` | `openthread/error.h` | Error code type |
| `openthread_state_changed_callback` | `openthread.h` | Callback struct for role changes |

---

## 3. nRF SDK Libraries

**Disk path:** `/home/jeremy/ncs/v3.2.2/nrf/`

### DK Buttons and LEDs

**Header:** `/home/jeremy/ncs/v3.2.2/nrf/include/dk_buttons_and_leds.h`
**Source:** `/home/jeremy/ncs/v3.2.2/nrf/lib/dk_buttons_and_leds/dk_buttons_and_leds.c`
**Kconfig:** `CONFIG_DK_LIBRARY=y`

| Symbol | Used in | Type | Description |
|--------|---------|------|-------------|
| `dk_leds_init()` | `main.c` | Function | Initialize LED GPIOs |
| `dk_set_led()` | `main.c` | Function | Set LED on/off (index, state) |
| `dk_set_led_on()` | `main.c` | Function | Turn LED on by index |
| `DK_LED1` | `main.c` | Constant | LED index 0 |
| `DK_LED2` | `main.c` | Constant | LED index 1 |

### CoAP Utils

**Header:** `/home/jeremy/ncs/v3.2.2/nrf/include/net/coap_utils.h`
**Source:** `/home/jeremy/ncs/v3.2.2/nrf/subsys/net/lib/coap_utils/coap_utils.c`
**Kconfig:** `CONFIG_COAP_UTILS=y`

| Symbol | Used in | Type | Description |
|--------|---------|------|-------------|
| `coap_init()` | `thread_coap.c` | Function | Initialize CoAP library (address family, callback) |
| `coap_send_request()` | `thread_coap.c` | Function | Send a CoAP request (method, dest, URI, payload, len, callback) |

---

## 4. Zephyr Subsystem APIs

**Disk path:** `/home/jeremy/ncs/v3.2.2/zephyr/include/zephyr/`

### Settings / NVS Persistent Storage

**Header:** `settings/settings.h`

| Symbol | Used in | Type | Description |
|--------|---------|------|-------------|
| `settings_subsys_init()` | `device_config.c` | Function | Initialize settings/NVS backend |
| `settings_load()` | `device_config.c` | Function | Load all settings (calls registered handlers) |
| `settings_save_one()` | `device_config.c` | Function | Save a single key-value pair to NVS |
| `settings_delete()` | `device_config.c` | Function | Delete a key from NVS |
| `SETTINGS_STATIC_HANDLER_DEFINE()` | `device_config.c` | Macro | Register a settings load/save handler at compile time |
| `settings_read_cb` | `device_config.c` | Type | Callback type for reading setting values during load |

### UART Driver

**Header:** `drivers/uart.h`

| Symbol | Used in | Type | Description |
|--------|---------|------|-------------|
| `uart_irq_callback_set()` | `uci_uart.c` | Function | Set UART interrupt callback |
| `uart_irq_rx_enable()` | `uci_uart.c` | Function | Enable RX interrupt |
| `uart_irq_update()` | `uci_uart.c` | Function | Acknowledge IRQ (must call first in ISR) |
| `uart_irq_rx_ready()` | `uci_uart.c` | Function | Check if RX data is available |
| `uart_fifo_read()` | `uci_uart.c` | Function | Read bytes from UART FIFO |
| `uart_poll_out()` | `uci_uart.c` | Function | Send one byte (polling/blocking) |

### Device Model

**Header:** `device.h`

| Symbol | Used in | Type | Description |
|--------|---------|------|-------------|
| `DEVICE_DT_GET()` | `uci_uart.c` | Macro | Get device struct from devicetree node |
| `device_is_ready()` | `uci_uart.c` | Function | Check if device is initialized and ready |
| `DT_CHOSEN()` | `uci_uart.c` | Macro | Get devicetree chosen node (e.g. `zephyr_console`) |

### Networking

**Header:** `net/socket.h`

| Symbol | Used in | Type | Description |
|--------|---------|------|-------------|
| `zsock_inet_pton()` | `thread_coap.c` | Function | Parse IPv6 address string to binary |
| `htons()` | `thread_coap.c` | Function | Host-to-network byte order (16-bit) |
| `AF_INET6` | `thread_coap.c` | Constant | IPv6 address family |
| `sockaddr_in6` | `thread_coap.c` | Type | IPv6 socket address struct |

**Header:** `net/coap.h`

| Symbol | Used in | Type | Description |
|--------|---------|------|-------------|
| `COAP_METHOD_POST` | `thread_coap.c` | Constant | CoAP POST method (Zephyr CoAP, not OT CoAP) |

### Reboot & Retention

**Header:** `sys/reboot.h`

| Symbol | Used in | Type | Description |
|--------|---------|------|-------------|
| `sys_reboot()` | `uci.c` | Function | System reboot (cold or warm) |
| `SYS_REBOOT_COLD` | `uci.c` | Constant | Cold reboot type |
| `SYS_REBOOT_WARM` | `uci.c` | Constant | Warm reboot type |

**Header:** `retention/bootmode.h`

| Symbol | Used in | Type | Description |
|--------|---------|------|-------------|
| `bootmode_set()` | `uci.c` | Function | Set boot mode flag in retention memory |
| `BOOT_MODE_TYPE_BOOTLOADER` | `uci.c` | Constant | Request MCUboot serial recovery on next boot |

Guarded by `#if defined(CONFIG_RETENTION_BOOT_MODE)`.

### Kernel Primitives

**Header:** `kernel.h`

#### Threads

| Symbol | Used in | Type | Description |
|--------|---------|------|-------------|
| `k_thread_create()` | `uwb_manager.c`, `uci_uart.c` | Function | Create and start a thread |
| `k_thread_name_set()` | `uwb_manager.c`, `uci_uart.c` | Function | Set thread name for debugging |
| `K_THREAD_STACK_DEFINE()` | `uwb_manager.c`, `uci_uart.c`, `thread_coap.c` | Macro | Statically define a thread stack |
| `K_THREAD_STACK_SIZEOF()` | `uwb_manager.c`, `uci_uart.c`, `thread_coap.c` | Macro | Get size of a defined stack |
| `k_thread` | `uwb_manager.c`, `uci_uart.c` | Type | Thread control block struct |

#### Semaphores

| Symbol | Used in | Type | Description |
|--------|---------|------|-------------|
| `K_SEM_DEFINE()` | `uwb_manager.c`, `uci_uart.c` | Macro | Statically define a semaphore |
| `k_sem_take()` | `uwb_manager.c`, `uci_uart.c` | Function | Take (wait on) a semaphore |
| `k_sem_give()` | `uwb_manager.c`, `uci_uart.c` | Function | Give (signal) a semaphore |

#### Mutexes

| Symbol | Used in | Type | Description |
|--------|---------|------|-------------|
| `K_MUTEX_DEFINE()` | `uci.c` | Macro | Statically define a mutex |
| `k_mutex_lock()` | `uci.c` | Function | Lock a mutex (blocking) |
| `k_mutex_unlock()` | `uci.c` | Function | Unlock a mutex |

#### Work Queues

| Symbol | Used in | Type | Description |
|--------|---------|------|-------------|
| `k_work_queue_init()` | `thread_coap.c` | Function | Initialize a work queue |
| `k_work_queue_start()` | `thread_coap.c` | Function | Start a work queue thread |
| `k_work_init()` | `thread_coap.c` | Function | Initialize a work item |
| `k_work_submit_to_queue()` | `thread_coap.c` | Function | Submit work item to a specific queue |
| `k_work_q` | `thread_coap.c` | Type | Work queue struct |
| `k_work` | `thread_coap.c` | Type | Work item struct |

#### Message Queues

| Symbol | Used in | Type | Description |
|--------|---------|------|-------------|
| `K_MSGQ_DEFINE()` | `thread_coap.c` | Macro | Statically define a message queue |
| `k_msgq_put()` | `thread_coap.c` | Function | Put a message into the queue |
| `k_msgq_get()` | `thread_coap.c` | Function | Get a message from the queue |

#### Atomics

**Header:** `sys/atomic.h`

| Symbol | Used in | Type | Description |
|--------|---------|------|-------------|
| `ATOMIC_INIT()` | `uwb_manager.c` | Macro | Statically initialize an atomic variable |
| `atomic_get()` | `uwb_manager.c` | Function | Atomically read a value |
| `atomic_set()` | `uwb_manager.c` | Function | Atomically write a value |
| `atomic_t` | `uwb_manager.c` | Type | Atomic integer type |

#### Timing & Sleep

| Symbol | Used in | Type | Description |
|--------|---------|------|-------------|
| `k_msleep()` | `main.c`, `uwb_manager.c`, `uci.c` | Function | Sleep for milliseconds |
| `k_sleep()` | `uwb_manager.c`, `uci_uart.c` | Function | Sleep for a `k_timeout_t` duration |
| `k_uptime_get()` | `thread_coap.c`, `uci.c`, `uwb_manager.c` | Function | Get ms since boot (int64) |
| `k_ticks_to_ms_floor64()` | `uwb_manager.c` | Function | Convert ticks to ms |
| `K_NO_WAIT` | `uwb_manager.c`, `thread_coap.c` | Constant | Zero timeout (non-blocking) |
| `K_FOREVER` | `uwb_manager.c`, `uci.c`, `uci_uart.c` | Constant | Infinite timeout |
| `K_MSEC()` | `uwb_manager.c`, `uci.c` | Macro | Create timeout from milliseconds |
| `K_USEC()` | `uwb_manager.c` | Macro | Create timeout from microseconds |
| `K_TIMEOUT_EQ()` | `uwb_manager.c` | Macro | Compare two timeouts for equality |
| `k_timeout_t` | `uwb_manager.c` | Type | Timeout value type |

#### Scheduler

| Symbol | Used in | Type | Description |
|--------|---------|------|-------------|
| `k_sched_lock()` | `uwb_manager.c` | Function | Lock scheduler (prevent preemption) |
| `k_sched_unlock()` | `uwb_manager.c` | Function | Unlock scheduler |

#### Logging

**Header:** `logging/log.h`

| Symbol | Used in | Type | Description |
|--------|---------|------|-------------|
| `LOG_MODULE_REGISTER()` | All `.c` files | Macro | Register a log module with name and level |
| `LOG_INF()` | All `.c` files | Macro | Info-level log message |
| `LOG_ERR()` | All `.c` files | Macro | Error-level log message |
| `LOG_WRN()` | All `.c` files | Macro | Warning-level log message |
| `LOG_DBG()` | `thread_coap.c` | Macro | Debug-level log message |

#### Misc

| Symbol | Used in | Header | Description |
|--------|---------|--------|-------------|
| `printk()` | `main.c` | `sys/printk.h` | Kernel-level printf (bypasses logging) |
| `ARG_UNUSED()` | Multiple files | `sys/util.h` | Suppress unused-parameter warnings |
| `IS_ENABLED()` | `device_config.c` | `sys/util.h` | Check if Kconfig symbol is enabled |
| `__packed` | `thread_coap.c` | Compiler attribute | Packed struct (no padding) |

---

## 5. Kconfig-Derived Symbols

These `CONFIG_*` macros are auto-generated from `firmware/Kconfig` + `firmware/prj.conf` + `firmware/boards/*.conf`. Not from any header — the build system defines them as compiler flags.

| Symbol | Used in | Defined by |
|--------|---------|------------|
| `CONFIG_BOARD` | `main.c` | Build system (board name string) |
| `CONFIG_NODE_ROLE_TAG` | `device_config.c` | `firmware/Kconfig` choice |
| `CONFIG_NODE_ROLE_ANCHOR` | `uwb_manager.c` | `firmware/Kconfig` choice |
| `CONFIG_UWB_NODE_SHORT_ADDR` | `device_config.c` | `firmware/Kconfig` |
| `CONFIG_UWB_RANGING_INTERVAL_MS` | `device_config.c` | `firmware/Kconfig` |
| `CONFIG_COAP_SERVER_ADDR` | `device_config.c` | `firmware/Kconfig` |
| `CONFIG_COAP_SERVER_PORT` | `device_config.c` | `firmware/Kconfig` |
| `CONFIG_RETENTION_BOOT_MODE` | `uci.c` | `firmware/prj.conf` |
| `CONFIG_MCUBOOT_BOOTLOADER_MODE_SINGLE_APP` | `CMakeLists.txt` | Board-specific sysbuild |
| `CONFIG_SINGLE_APPLICATION_SLOT` | `CMakeLists.txt` | Workaround define |
