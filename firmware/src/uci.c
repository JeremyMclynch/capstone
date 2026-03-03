/*
 * UCI Command Processor
 *
 * Dispatches incoming UCI request frames to handler functions and
 * builds response frames. Transport-agnostic — can be driven by
 * UART, CoAP, or any other byte-oriented channel.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#if defined(CONFIG_RETENTION_BOOT_MODE)
#include <zephyr/retention/bootmode.h>
#endif
#include <string.h>

#include "uci.h"
#include "device_config.h"
#include "uwb_manager.h"

LOG_MODULE_REGISTER(uci, LOG_LEVEL_INF);

/* Mutex for thread-safe access from multiple transports */
static K_MUTEX_DEFINE(uci_lock);

/* Firmware version (major.minor) */
#define FW_VERSION_MAJOR 1
#define FW_VERSION_MINOR 0

/* ── CRC-8 (polynomial 0x07) ─────────────────────────────────────── */

uint8_t uci_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc & 0x80) ? ((crc << 1) ^ 0x07) : (crc << 1);
        }
    }
    return crc;
}

/* ── Helper: set simple OK/error response ────────────────────────── */

static void rsp_ok(struct uci_response *rsp, uint8_t cmd)
{
    rsp->cmd    = cmd;
    rsp->status = UCI_STATUS_OK;
    rsp->len    = 0;
}

static void rsp_err(struct uci_response *rsp, uint8_t cmd, uint8_t status)
{
    rsp->cmd    = cmd;
    rsp->status = status;
    rsp->len    = 0;
}

/* ── Command handlers ────────────────────────────────────────────── */

static void handle_get_info(const struct uci_request *req,
                            struct uci_response *rsp)
{
    rsp->cmd    = req->cmd;
    rsp->status = UCI_STATUS_OK;

    struct uwb_status st;
    uwb_manager_get_status(&st);

    /* version_major(1) + version_minor(1) + role(1) + addr(2) + state(1) = 6 */
    rsp->payload[0] = FW_VERSION_MAJOR;
    rsp->payload[1] = FW_VERSION_MINOR;
    rsp->payload[2] = g_config.role;
    rsp->payload[3] = (uint8_t)(g_config.uwb_addr & 0xFF);
    rsp->payload[4] = (uint8_t)(g_config.uwb_addr >> 8);
    rsp->payload[5] = st.running ? 1 : 0;
    rsp->len = 6;
}

static void handle_set_role(const struct uci_request *req,
                            struct uci_response *rsp)
{
    if (req->len != 1) {
        rsp_err(rsp, req->cmd, UCI_STATUS_ERR_BAD_PAYLOAD);
        return;
    }

    uint8_t role = req->payload[0];
    if (role != ROLE_ANCHOR && role != ROLE_TAG) {
        rsp_err(rsp, req->cmd, UCI_STATUS_ERR_INVALID_VAL);
        return;
    }

    g_config.role = role;
    LOG_INF("Role set to %s (save + reboot to apply)",
            role == ROLE_TAG ? "TAG" : "ANCHOR");
    rsp_ok(rsp, req->cmd);
}

static void handle_set_addr(const struct uci_request *req,
                            struct uci_response *rsp)
{
    if (req->len != 2) {
        rsp_err(rsp, req->cmd, UCI_STATUS_ERR_BAD_PAYLOAD);
        return;
    }

    uint16_t addr = (uint16_t)req->payload[0] | ((uint16_t)req->payload[1] << 8);
    if (addr == 0x0000 || addr == 0xFFFF) {
        rsp_err(rsp, req->cmd, UCI_STATUS_ERR_INVALID_VAL);
        return;
    }

    g_config.uwb_addr = addr;
    LOG_INF("UWB address set to 0x%04X", addr);
    rsp_ok(rsp, req->cmd);
}

static void handle_set_interval(const struct uci_request *req,
                                struct uci_response *rsp)
{
    if (req->len != 2) {
        rsp_err(rsp, req->cmd, UCI_STATUS_ERR_BAD_PAYLOAD);
        return;
    }

    uint16_t ms = (uint16_t)req->payload[0] | ((uint16_t)req->payload[1] << 8);
    if (ms < 50 || ms > 10000) {
        rsp_err(rsp, req->cmd, UCI_STATUS_ERR_INVALID_VAL);
        return;
    }

    g_config.ranging_interval_ms = ms;
    uwb_manager_set_interval(ms);
    LOG_INF("Ranging interval set to %u ms", ms);
    rsp_ok(rsp, req->cmd);
}

static void handle_set_server(const struct uci_request *req,
                              struct uci_response *rsp)
{
    /* payload: 16 bytes IPv6 address (binary) + 2 bytes port (LE) */
    if (req->len < 3 || req->len > SERVER_ADDR_MAX_LEN + 2) {
        rsp_err(rsp, req->cmd, UCI_STATUS_ERR_BAD_PAYLOAD);
        return;
    }

    /* payload is: ipv6_string (null-terminated) + port(2B LE) */
    uint8_t str_len = req->len - 2;
    if (str_len == 0 || req->payload[str_len - 1] != '\0') {
        rsp_err(rsp, req->cmd, UCI_STATUS_ERR_BAD_PAYLOAD);
        return;
    }

    uint16_t port = (uint16_t)req->payload[str_len] |
                    ((uint16_t)req->payload[str_len + 1] << 8);

    memcpy(g_config.server_addr, req->payload, str_len);
    g_config.server_port = port;

    LOG_INF("Server set to [%s]:%u", g_config.server_addr, port);
    rsp_ok(rsp, req->cmd);
}

static void handle_start(const struct uci_request *req,
                         struct uci_response *rsp)
{
    struct uwb_status st;
    uwb_manager_get_status(&st);

    if (st.running) {
        rsp_err(rsp, req->cmd, UCI_STATUS_ERR_BUSY);
        return;
    }

    int ret = uwb_manager_start();
    if (ret) {
        rsp_err(rsp, req->cmd, UCI_STATUS_ERR_BUSY);
        return;
    }

    LOG_INF("Ranging started");
    rsp_ok(rsp, req->cmd);
}

static void handle_stop(const struct uci_request *req,
                        struct uci_response *rsp)
{
    uwb_manager_stop();
    LOG_INF("Ranging stopped");
    rsp_ok(rsp, req->cmd);
}

static void handle_get_status(const struct uci_request *req,
                              struct uci_response *rsp)
{
    struct uwb_status st;
    uwb_manager_get_status(&st);

    rsp->cmd    = req->cmd;
    rsp->status = UCI_STATUS_OK;

    /* state(1) + last_dist_mm(4) + uptime_s(4) + range_count(4) = 13 */
    rsp->payload[0] = st.running ? 1 : 0;

    uint32_t dist_mm = st.last_distance_mm;
    rsp->payload[1] = (uint8_t)(dist_mm);
    rsp->payload[2] = (uint8_t)(dist_mm >> 8);
    rsp->payload[3] = (uint8_t)(dist_mm >> 16);
    rsp->payload[4] = (uint8_t)(dist_mm >> 24);

    uint32_t uptime = (uint32_t)(k_uptime_get() / 1000);
    rsp->payload[5] = (uint8_t)(uptime);
    rsp->payload[6] = (uint8_t)(uptime >> 8);
    rsp->payload[7] = (uint8_t)(uptime >> 16);
    rsp->payload[8] = (uint8_t)(uptime >> 24);

    uint32_t count = st.range_count;
    rsp->payload[9]  = (uint8_t)(count);
    rsp->payload[10] = (uint8_t)(count >> 8);
    rsp->payload[11] = (uint8_t)(count >> 16);
    rsp->payload[12] = (uint8_t)(count >> 24);

    rsp->len = 13;
}

static void handle_save_config(const struct uci_request *req,
                               struct uci_response *rsp)
{
    int ret = device_config_save();
    if (ret) {
        rsp_err(rsp, req->cmd, UCI_STATUS_ERR_BUSY);
    } else {
        rsp_ok(rsp, req->cmd);
    }
}

static void handle_factory_reset(const struct uci_request *req,
                                 struct uci_response *rsp)
{
    device_config_reset();
    rsp_ok(rsp, req->cmd);

    /* Schedule a reboot after the response is sent */
    LOG_INF("Factory reset — rebooting...");
    k_sleep(K_MSEC(100));
    sys_reboot(SYS_REBOOT_COLD);
}

static void handle_enter_bootloader(const struct uci_request *req,
                                    struct uci_response *rsp)
{
#if defined(CONFIG_RETENTION_BOOT_MODE)
    int ret = bootmode_set(BOOT_MODE_TYPE_BOOTLOADER);
    if (ret) {
        LOG_ERR("Failed to set boot mode: %d", ret);
        rsp_err(rsp, req->cmd, UCI_STATUS_ERR_BUSY);
        return;
    }

    rsp_ok(rsp, req->cmd);

    /* Let the response transmit before rebooting */
    LOG_INF("Entering MCUboot serial recovery...");
    k_sleep(K_MSEC(100));
    sys_reboot(SYS_REBOOT_WARM);
#elif defined(CONFIG_BOARD_XIAO_BLE)
    /* Adafruit UF2 bootloader: write magic to GPREGRET and reset */
    NRF_POWER->GPREGRET = 0x57; /* DFU_MAGIC_UF2_RESET */
    rsp_ok(rsp, req->cmd);
    LOG_INF("Entering UF2 bootloader...");
    k_sleep(K_MSEC(100));
    sys_reboot(SYS_REBOOT_WARM);
#else
    LOG_WRN("Bootloader entry not supported on this board");
    rsp_err(rsp, req->cmd, UCI_STATUS_ERR_UNKNOWN_CMD);
#endif
}

static void handle_reboot(const struct uci_request *req,
                          struct uci_response *rsp)
{
    rsp_ok(rsp, req->cmd);

    LOG_INF("Reboot requested via UCI");
    k_sleep(K_MSEC(100));
    sys_reboot(SYS_REBOOT_COLD);
}

static void handle_calibrate(const struct uci_request *req,
                             struct uci_response *rsp)
{
    /* Only anchors compute distance — calibrate is meaningless on tags */
    if (g_config.role != ROLE_ANCHOR) {
        LOG_WRN("Calibrate rejected: only anchors compute distance");
        rsp_err(rsp, req->cmd, UCI_STATUS_ERR_INVALID_VAL);
        return;
    }

    struct uwb_status st;
    uwb_manager_get_status(&st);

    if (!st.running || st.range_count == 0) {
        rsp_err(rsp, req->cmd, UCI_STATUS_ERR_BUSY);
        return;
    }

    /* Get raw distance by removing current offset */
    int32_t raw_mm = (int32_t)st.last_distance_mm - g_config.calibration_offset_mm;
    int16_t new_offset = (int16_t)(1000 - raw_mm);

    g_config.calibration_offset_mm = new_offset;

    LOG_INF("Calibrated: raw=%d mm, offset=%d mm", raw_mm, (int)new_offset);

    rsp->cmd = req->cmd;
    rsp->status = UCI_STATUS_OK;
    rsp->payload[0] = (uint8_t)(new_offset & 0xFF);
    rsp->payload[1] = (uint8_t)((new_offset >> 8) & 0xFF);
    rsp->len = 2;
}

static void handle_set_cal_offset(const struct uci_request *req,
                                   struct uci_response *rsp)
{
    if (req->len != 2) {
        rsp_err(rsp, req->cmd, UCI_STATUS_ERR_BAD_PAYLOAD);
        return;
    }

    int16_t offset = (int16_t)((uint16_t)req->payload[0] |
                               ((uint16_t)req->payload[1] << 8));
    g_config.calibration_offset_mm = offset;

    LOG_INF("Calibration offset set to %d mm", (int)offset);
    rsp_ok(rsp, req->cmd);
}

static void handle_get_cal_offset(const struct uci_request *req,
                                   struct uci_response *rsp)
{
    int16_t offset = g_config.calibration_offset_mm;

    rsp->cmd = req->cmd;
    rsp->status = UCI_STATUS_OK;
    rsp->payload[0] = (uint8_t)(offset & 0xFF);
    rsp->payload[1] = (uint8_t)((offset >> 8) & 0xFF);
    rsp->len = 2;
}

/* ── Dispatcher ───────────────────────────────────────────────────── */

void uci_process(const struct uci_request *req, struct uci_response *rsp)
{
    switch (req->cmd) {
    case UCI_CMD_GET_INFO:      handle_get_info(req, rsp);      break;
    case UCI_CMD_SET_ROLE:      handle_set_role(req, rsp);      break;
    case UCI_CMD_SET_ADDR:      handle_set_addr(req, rsp);      break;
    case UCI_CMD_SET_INTERVAL:  handle_set_interval(req, rsp);  break;
    case UCI_CMD_SET_SERVER:    handle_set_server(req, rsp);     break;
    case UCI_CMD_START:         handle_start(req, rsp);          break;
    case UCI_CMD_STOP:          handle_stop(req, rsp);           break;
    case UCI_CMD_GET_STATUS:    handle_get_status(req, rsp);     break;
    case UCI_CMD_SAVE_CONFIG:   handle_save_config(req, rsp);    break;
    case UCI_CMD_FACTORY_RESET: handle_factory_reset(req, rsp);  break;
    case UCI_CMD_ENTER_BOOTLOADER: handle_enter_bootloader(req, rsp); break;
    case UCI_CMD_REBOOT:        handle_reboot(req, rsp);             break;
    case UCI_CMD_CALIBRATE:     handle_calibrate(req, rsp);          break;
    case UCI_CMD_SET_CAL_OFFSET: handle_set_cal_offset(req, rsp);   break;
    case UCI_CMD_GET_CAL_OFFSET: handle_get_cal_offset(req, rsp);   break;
    default:
        LOG_WRN("Unknown UCI cmd 0x%02X", req->cmd);
        rsp_err(rsp, req->cmd, UCI_STATUS_ERR_UNKNOWN_CMD);
        break;
    }
}

/* ── Thread-safe wrapper ─────────────────────────────────────────── */

void uci_process_locked(const struct uci_request *req, struct uci_response *rsp)
{
    k_mutex_lock(&uci_lock, K_FOREVER);
    uci_process(req, rsp);
    k_mutex_unlock(&uci_lock);
}

/* ── Serialize response into TX buffer ───────────────────────────── */

int uci_serialize_response(const struct uci_response *rsp,
                           uint8_t *buf, size_t buf_size)
{
    size_t total = 4 + rsp->len + 1;  /* sync + cmd + status + len + payload + crc */
    if (buf_size < total) {
        return -1;
    }

    buf[0] = UCI_SYNC_RSP;
    buf[1] = rsp->cmd;
    buf[2] = rsp->status;
    buf[3] = rsp->len;
    if (rsp->len > 0) {
        memcpy(&buf[4], rsp->payload, rsp->len);
    }
    buf[4 + rsp->len] = uci_crc8(buf, 4 + rsp->len);

    return (int)total;
}
