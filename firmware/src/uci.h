#ifndef UCI_H
#define UCI_H

#include <stdint.h>
#include <stddef.h>

/* ── Frame format ────────────────────────────────────────────────── */
/*
 * Request:  [SYNC_REQ=0xAA] [CMD:1B] [LEN:1B] [PAYLOAD:0..252B] [CRC8:1B]
 * Response: [SYNC_RSP=0xBB] [CMD:1B] [STATUS:1B] [LEN:1B] [PAYLOAD:0..251B] [CRC8:1B]
 */

#define UCI_SYNC_REQ        0xAA
#define UCI_SYNC_RSP        0xBB
#define UCI_MAX_PAYLOAD     64   /* practical max — keeps buffers small */
#define UCI_FRAME_OVERHEAD  3    /* sync + cmd + len (request) */

/* ── Command opcodes ─────────────────────────────────────────────── */

#define UCI_CMD_GET_INFO        0x01
#define UCI_CMD_SET_ROLE        0x02
#define UCI_CMD_SET_ADDR        0x03
#define UCI_CMD_SET_INTERVAL    0x04
#define UCI_CMD_SET_SERVER      0x05
#define UCI_CMD_START           0x10
#define UCI_CMD_STOP            0x11
#define UCI_CMD_GET_STATUS      0x12
#define UCI_CMD_SAVE_CONFIG     0x20
#define UCI_CMD_FACTORY_RESET   0x21
#define UCI_CMD_ENTER_BOOTLOADER 0x22
#define UCI_CMD_REBOOT          0x23

/* ── Status codes ────────────────────────────────────────────────── */

#define UCI_STATUS_OK               0x00
#define UCI_STATUS_ERR_UNKNOWN_CMD  0x01
#define UCI_STATUS_ERR_BAD_PAYLOAD  0x02
#define UCI_STATUS_ERR_INVALID_VAL  0x03
#define UCI_STATUS_ERR_BUSY         0x04

/* ── Parsed request frame ────────────────────────────────────────── */

struct uci_request {
    uint8_t cmd;
    uint8_t len;
    uint8_t payload[UCI_MAX_PAYLOAD];
};

/* ── Response frame ──────────────────────────────────────────────── */

struct uci_response {
    uint8_t cmd;
    uint8_t status;
    uint8_t len;
    uint8_t payload[UCI_MAX_PAYLOAD];
};

/* ── CRC-8 (polynomial 0x07, init 0x00) ──────────────────────────── */

uint8_t uci_crc8(const uint8_t *data, size_t len);

/**
 * @brief Process a received UCI request and build a response.
 *
 * @param req  Parsed request (cmd, len, payload).
 * @param rsp  Output response (filled by this function).
 */
void uci_process(const struct uci_request *req, struct uci_response *rsp);

/**
 * @brief Thread-safe wrapper around uci_process().
 *
 * All transports (UART, CoAP) should call this to avoid concurrent access.
 */
void uci_process_locked(const struct uci_request *req, struct uci_response *rsp);

/**
 * @brief Serialize a response into a byte buffer for transmission.
 *
 * @param rsp      Response to serialize.
 * @param buf      Output buffer (must be at least rsp->len + 5 bytes).
 * @param buf_size Size of output buffer.
 * @return Number of bytes written, or -1 if buffer too small.
 */
int uci_serialize_response(const struct uci_response *rsp,
                           uint8_t *buf, size_t buf_size);

#endif /* UCI_H */
