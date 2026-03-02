/*
 * UCI UART Transport
 *
 * Receives UCI binary frames from UART using interrupt-driven RX.
 * A dedicated thread runs a state machine that assembles bytes into
 * complete frames, validates CRC, dispatches to uci_process(), and
 * sends the response back over the same UART.
 *
 * Frame format:
 *   [0xAA] [CMD] [LEN] [PAYLOAD...] [CRC8]
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "uci.h"

LOG_MODULE_REGISTER(uci_uart, LOG_LEVEL_INF);

/* ── UART device ─────────────────────────────────────────────────── */

static const struct device *uart_dev;

/* ── RX ring buffer ──────────────────────────────────────────────── */

#define RX_RING_SIZE 128
static uint8_t rx_ring[RX_RING_SIZE];
static volatile uint16_t rx_head;
static volatile uint16_t rx_tail;

static K_SEM_DEFINE(rx_sem, 0, 1);

/* ── State machine states ────────────────────────────────────────── */

enum rx_state {
    STATE_WAIT_SYNC,
    STATE_READ_CMD,
    STATE_READ_LEN,
    STATE_READ_PAYLOAD,
    STATE_READ_CRC,
};

/* ── Thread ──────────────────────────────────────────────────────── */

#define UCI_UART_STACK_SIZE 1536
#define UCI_UART_PRIORITY   8

K_THREAD_STACK_DEFINE(uci_uart_stack, UCI_UART_STACK_SIZE);
static struct k_thread uci_uart_thread_data;

/* ── Ring buffer helpers ─────────────────────────────────────────── */

static inline bool ring_empty(void)
{
    return rx_head == rx_tail;
}

static inline bool ring_put(uint8_t b)
{
    uint16_t next = (rx_head + 1) % RX_RING_SIZE;
    if (next == rx_tail) return false;  /* full */
    rx_ring[rx_head] = b;
    rx_head = next;
    return true;
}

static inline bool ring_get(uint8_t *b)
{
    if (ring_empty()) return false;
    *b = rx_ring[rx_tail];
    rx_tail = (rx_tail + 1) % RX_RING_SIZE;
    return true;
}

/* ── UART ISR callback ───────────────────────────────────────────── */

static void uart_isr_callback(const struct device *dev, void *user_data)
{
    ARG_UNUSED(user_data);

    if (!uart_irq_update(dev)) return;

    if (uart_irq_rx_ready(dev)) {
        uint8_t buf[16];
        int len = uart_fifo_read(dev, buf, sizeof(buf));
        for (int i = 0; i < len; i++) {
            ring_put(buf[i]);
        }
        k_sem_give(&rx_sem);
    }
}

/* ── Read one byte (blocking) ────────────────────────────────────── */

static uint8_t read_byte(void)
{
    uint8_t b;
    while (!ring_get(&b)) {
        k_sem_take(&rx_sem, K_FOREVER);
    }
    return b;
}

/* ── Send response bytes ─────────────────────────────────────────── */

static void send_response(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uart_poll_out(uart_dev, buf[i]);
    }
}

/* ── UCI UART thread ─────────────────────────────────────────────── */

static void uci_uart_thread_entry(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    LOG_INF("UCI UART ready on %s", uart_dev->name);

    enum rx_state state = STATE_WAIT_SYNC;
    struct uci_request req;
    uint8_t frame_buf[UCI_FRAME_OVERHEAD + UCI_MAX_PAYLOAD + 1];
    uint8_t frame_pos = 0;
    uint8_t payload_remaining = 0;

    while (1) {
        uint8_t b = read_byte();

        switch (state) {
        case STATE_WAIT_SYNC:
            if (b == UCI_SYNC_REQ) {
                frame_buf[0] = b;
                frame_pos = 1;
                state = STATE_READ_CMD;
            }
            break;

        case STATE_READ_CMD:
            req.cmd = b;
            frame_buf[frame_pos++] = b;
            state = STATE_READ_LEN;
            break;

        case STATE_READ_LEN:
            req.len = b;
            frame_buf[frame_pos++] = b;
            if (b > UCI_MAX_PAYLOAD) {
                LOG_WRN("UCI payload too large: %u", b);
                state = STATE_WAIT_SYNC;
                break;
            }
            if (b == 0) {
                state = STATE_READ_CRC;
            } else {
                payload_remaining = b;
                state = STATE_READ_PAYLOAD;
            }
            break;

        case STATE_READ_PAYLOAD:
            req.payload[req.len - payload_remaining] = b;
            frame_buf[frame_pos++] = b;
            payload_remaining--;
            if (payload_remaining == 0) {
                state = STATE_READ_CRC;
            }
            break;

        case STATE_READ_CRC: {
            uint8_t expected_crc = uci_crc8(frame_buf, frame_pos);
            if (b != expected_crc) {
                LOG_WRN("UCI CRC mismatch: got 0x%02X expected 0x%02X", b, expected_crc);
                state = STATE_WAIT_SYNC;
                break;
            }

            /* Valid frame — process it */
            struct uci_response rsp;
            uci_process_locked(&req, &rsp);

            /* Serialize and send response */
            uint8_t tx_buf[UCI_MAX_PAYLOAD + 8];
            int tx_len = uci_serialize_response(&rsp, tx_buf, sizeof(tx_buf));
            if (tx_len > 0) {
                send_response(tx_buf, tx_len);
            }

            state = STATE_WAIT_SYNC;
            break;
        }
        }
    }
}

/* ── Public API ───────────────────────────────────────────────────── */

int uci_uart_init(void)
{
    uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
    if (!device_is_ready(uart_dev)) {
        LOG_ERR("UART device not ready");
        return -ENODEV;
    }

    /* Set up interrupt-driven RX */
    uart_irq_callback_set(uart_dev, uart_isr_callback);
    uart_irq_rx_enable(uart_dev);

    /* Start the processing thread */
    k_thread_create(&uci_uart_thread_data, uci_uart_stack,
                    K_THREAD_STACK_SIZEOF(uci_uart_stack),
                    uci_uart_thread_entry, NULL, NULL, NULL,
                    UCI_UART_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&uci_uart_thread_data, "uci_uart");

    return 0;
}
