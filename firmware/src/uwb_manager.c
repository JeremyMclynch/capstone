/*
 * UWB Manager - DS-TWR Ranging Implementation
 *
 * Implements Double-Sided Two-Way Ranging (DS-TWR) using the DW3000
 * UWB transceiver via the zephyr-dw3000-decadriver Zephyr module.
 *
 * Anchor role (responder): Waits for POLL frames from any tag,
 *   performs the DS-TWR exchange, calculates distance, invokes callback.
 *
 * Tag role (initiator): Cycles through the known anchor address list,
 *   initiates a DS-TWR exchange with each anchor in turn.
 *
 * Frame format (IEEE 802.15.4 compliant, 16-bit addressing):
 *   [0-1]  Frame control  (0x8841 = data, 16-bit addr, intra-PAN)
 *   [2]    Sequence number
 *   [3-4]  PAN ID         (CONFIG_UWB_PAN_ID, little-endian)
 *   [5-6]  Destination address (16-bit, little-endian)
 *   [7-8]  Source address      (16-bit, little-endian)
 *   [9]    Function code  (POLL=0x21, RESP=0x10, FINAL=0x23)
 *   [10+]  Payload (timestamps in FINAL; activity code in RESP)
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <math.h>

#include "deca_probe_interface.h"
#include "deca_device_api.h"
#include "dw3000_hw.h"
#include "dw3000_spi.h"

#include "uwb_manager.h"

LOG_MODULE_REGISTER(uwb_manager, LOG_LEVEL_INF);

/* ── Configuration ────────────────────────────────────────────────── */

/* Known anchor short addresses (tags only).
 * Edit to match your deployment. */
static const uint16_t anchor_list[UWB_MAX_ANCHORS] = {
    0x0001,  /* Anchor 1 */
    0x0002,  /* Anchor 2 */
    0x0003,  /* Anchor 3 */
    /* Add more anchors here, up to UWB_MAX_ANCHORS */
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000  /* Sentinel: 0x0000 = unused */
};

/* DW3000 channel/PHY configuration (matching examples ex_05a/ex_05b) */
static dwt_config_t uwb_config = {
    5,               /* Channel 5 */
    DWT_PLEN_128,    /* Preamble length 128 symbols */
    DWT_PAC8,        /* PAC size 8 (for 128-symbol preamble) */
    9,               /* TX preamble code 9 */
    9,               /* RX preamble code 9 */
    1,               /* Non-standard 8-symbol SFD */
    DWT_BR_6M8,      /* 6.8 Mbps data rate */
    DWT_PHRMODE_STD, /* Standard PHR mode */
    DWT_PHRRATE_STD, /* Standard PHR rate */
    (129 + 8 - 8),   /* SFD timeout = preamble_len + 1 + SFD_len - PAC */
    DWT_STS_MODE_OFF,
    DWT_STS_LEN_64,
    DWT_PDOA_M0
};

/* TX power / spectrum calibration (typical values for Ch.5) */
static dwt_txconfig_t txconfig = {
    0x34,       /* PG delay */
    0xfdfdfdfd, /* TX power */
    0x0         /* PG count (0 = auto) */
};

/* ── Timing constants (UWB microseconds) ─────────────────────────── */
#define CPU_PROCESSING_TIME         400
#define POLL_TX_TO_RESP_RX_DLY_UUS  (300 + CPU_PROCESSING_TIME)
#define RESP_RX_TO_FINAL_TX_DLY_UUS (300 + CPU_PROCESSING_TIME)
#define POLL_RX_TO_RESP_TX_DLY_UUS  900
#define RESP_TX_TO_FINAL_RX_DLY_UUS 500
#define RESP_RX_TIMEOUT_UUS         300
#define FINAL_RX_TIMEOUT_UUS        220
#define PRE_TIMEOUT                 5
#define UUS_TO_DWT_TIME             65536   /* 1 UWB-us = 65536 DWT time units */

/* Speed of light in m/s */
#define SPEED_OF_LIGHT 299702547.0

/* ── Frame templates ──────────────────────────────────────────────── */
#define FRAME_HDR_LEN     10  /* bytes before function-specific payload */
#define ADDR_DST_IDX       5
#define ADDR_SRC_IDX       7
#define SEQ_IDX            2
#define FUNC_IDX           9

/* FINAL message timestamp field indices */
#define FINAL_POLL_TX_TS_IDX  10
#define FINAL_RESP_RX_TS_IDX  14
#define FINAL_TX_TS_IDX       18
#define FINAL_MSG_LEN         22  /* 10 header + 12 timestamps */

static uint8_t tx_poll_msg[]  = { 0x41, 0x88, 0, 0xCA, 0xDE,
                                   0xFF, 0xFF,         /* dst: broadcast */
                                   0x00, 0x00,         /* src: filled at init */
                                   UWB_FC_POLL };

static uint8_t tx_resp_msg[]  = { 0x41, 0x88, 0, 0xCA, 0xDE,
                                   0x00, 0x00,         /* dst: filled per exchange */
                                   0x00, 0x00,         /* src: filled at init */
                                   UWB_FC_RESP, 0x02, 0x00, 0x00 };

static uint8_t tx_final_msg[] = { 0x41, 0x88, 0, 0xCA, 0xDE,
                                   0x00, 0x00,         /* dst: anchor addr */
                                   0x00, 0x00,         /* src: filled at init */
                                   UWB_FC_FINAL,
                                   0,0,0,0,            /* poll TX ts */
                                   0,0,0,0,            /* resp RX ts */
                                   0,0,0,0 };          /* final TX ts */

#define RX_BUF_LEN 24
static uint8_t rx_buffer[RX_BUF_LEN];

static uint8_t frame_seq_nb = 0;
static uint16_t my_addr;
static uwb_distance_cb_t distance_cb;

/* ── UWB thread ───────────────────────────────────────────────────── */
#define UWB_THREAD_STACK_SIZE 2048
#define UWB_THREAD_PRIORITY   5

K_THREAD_STACK_DEFINE(uwb_thread_stack, UWB_THREAD_STACK_SIZE);
static struct k_thread uwb_thread_data;

/* ── Helper: read 40-bit TX timestamp ─────────────────────────────── */
static uint64_t get_tx_timestamp_u64(void)
{
    uint8_t ts_tab[5];
    dwt_readtxtimestamp(ts_tab);
    uint64_t ts = 0;
    for (int i = 4; i >= 0; i--) {
        ts = (ts << 8) | ts_tab[i];
    }
    return ts;
}

/* ── Helper: read 40-bit RX timestamp ─────────────────────────────── */
static uint64_t get_rx_timestamp_u64(void)
{
    uint8_t ts_tab[5];
    dwt_readrxtimestamp(ts_tab, DWT_IP_M); /* DWT_IP_M = Ipatov main (non-STS mode) */
    uint64_t ts = 0;
    for (int i = 4; i >= 0; i--) {
        ts = (ts << 8) | ts_tab[i];
    }
    return ts;
}

/* ── Helper: store 4-byte little-endian timestamp in frame ─────────── */
static void ts_set_32(uint8_t *field, uint64_t ts)
{
    uint32_t ts32 = (uint32_t)ts;
    field[0] = (uint8_t)(ts32);
    field[1] = (uint8_t)(ts32 >> 8);
    field[2] = (uint8_t)(ts32 >> 16);
    field[3] = (uint8_t)(ts32 >> 24);
}

/* ── Helper: read 4-byte little-endian timestamp from frame ─────────── */
static void ts_get_32(const uint8_t *field, uint32_t *ts)
{
    *ts = (uint32_t)field[0]
        | ((uint32_t)field[1] << 8)
        | ((uint32_t)field[2] << 16)
        | ((uint32_t)field[3] << 24);
}

/* ── Helper: poll DW3000 status register until bits set ──────────── */
static void wait_for_status(uint32_t *result, uint32_t mask)
{
    uint32_t reg;
    do {
        reg = dwt_readsysstatuslo();
    } while (!(reg & mask));
    if (result) {
        *result = reg;
    }
}

/* ── Helper: write 16-bit address into frame (little-endian) ──────── */
static void frame_set_addr(uint8_t *frame, int idx, uint16_t addr)
{
    frame[idx]     = (uint8_t)(addr & 0xFF);
    frame[idx + 1] = (uint8_t)(addr >> 8);
}

/* ── Helper: read 16-bit address from frame (little-endian) ─────────*/
static uint16_t frame_get_addr(const uint8_t *frame, int idx)
{
    return (uint16_t)frame[idx] | ((uint16_t)frame[idx + 1] << 8);
}

/* ── DS-TWR Anchor (Responder) Logic ─────────────────────────────── */
static void anchor_ranging_loop(void)
{
    LOG_INF("UWB Anchor 0x%04X: Listening for tags", my_addr);

    while (1) {
        uint32_t status_reg;

        /* Enable receiver with no timeout - wait indefinitely for a POLL */
        dwt_setpreambledetecttimeout(0);
        dwt_setrxtimeout(0);
        dwt_rxenable(DWT_START_RX_IMMEDIATE);

        wait_for_status(&status_reg,
            DWT_INT_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);

        if (!(status_reg & DWT_INT_RXFCG_BIT_MASK)) {
            dwt_writesysstatuslo(SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
            continue;
        }

        dwt_writesysstatuslo(DWT_INT_RXFCG_BIT_MASK);

        uint16_t frame_len = dwt_getframelength(NULL);
        if (frame_len > RX_BUF_LEN) {
            continue;
        }
        dwt_readrxdata(rx_buffer, frame_len, 0);

        /* Validate: must be addressed to us or broadcast, function = POLL */
        uint16_t dst = frame_get_addr(rx_buffer, ADDR_DST_IDX);
        if (dst != my_addr && dst != UWB_BROADCAST_ADDR) {
            continue;
        }
        if (rx_buffer[FUNC_IDX] != UWB_FC_POLL) {
            continue;
        }

        uint16_t tag_addr = frame_get_addr(rx_buffer, ADDR_SRC_IDX);
        uint64_t poll_rx_ts = get_rx_timestamp_u64();

        /* Schedule delayed response */
        uint32_t resp_tx_time =
            (poll_rx_ts + (POLL_RX_TO_RESP_TX_DLY_UUS * UUS_TO_DWT_TIME)) >> 8;
        dwt_setdelayedtrxtime(resp_tx_time);

        dwt_setrxaftertxdelay(RESP_TX_TO_FINAL_RX_DLY_UUS);
        dwt_setrxtimeout(FINAL_RX_TIMEOUT_UUS);
        dwt_setpreambledetecttimeout(PRE_TIMEOUT);

        /* Build RESP frame: dst=tag, src=anchor */
        tx_resp_msg[SEQ_IDX] = frame_seq_nb;
        frame_set_addr(tx_resp_msg, ADDR_DST_IDX, tag_addr);
        frame_set_addr(tx_resp_msg, ADDR_SRC_IDX, my_addr);

        dwt_writetxdata(sizeof(tx_resp_msg), tx_resp_msg, 0);
        dwt_writetxfctrl(sizeof(tx_resp_msg) + FCS_LEN, 0, 1);

        int ret = dwt_starttx(DWT_START_TX_DELAYED | DWT_RESPONSE_EXPECTED);
        if (ret == DWT_ERROR) {
            LOG_WRN("RESP TX too late, retrying");
            continue;
        }

        /* Wait for FINAL from tag */
        wait_for_status(&status_reg,
            DWT_INT_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
        frame_seq_nb++;

        if (!(status_reg & DWT_INT_RXFCG_BIT_MASK)) {
            dwt_writesysstatuslo(SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
            continue;
        }

        dwt_writesysstatuslo(DWT_INT_RXFCG_BIT_MASK | DWT_INT_TXFRS_BIT_MASK);

        frame_len = dwt_getframelength(NULL);
        if (frame_len > RX_BUF_LEN) {
            continue;
        }
        dwt_readrxdata(rx_buffer, frame_len, 0);

        /* Validate FINAL: from same tag, to us */
        dst = frame_get_addr(rx_buffer, ADDR_DST_IDX);
        uint16_t src = frame_get_addr(rx_buffer, ADDR_SRC_IDX);
        if (dst != my_addr || src != tag_addr || rx_buffer[FUNC_IDX] != UWB_FC_FINAL) {
            continue;
        }

        /* Retrieve response TX and final RX timestamps */
        uint64_t resp_tx_ts = get_tx_timestamp_u64();
        uint64_t final_rx_ts = get_rx_timestamp_u64();

        /* Extract initiator's timestamps from FINAL payload */
        uint32_t poll_tx_ts_i, resp_rx_ts_i, final_tx_ts_i;
        ts_get_32(&rx_buffer[FINAL_POLL_TX_TS_IDX], &poll_tx_ts_i);
        ts_get_32(&rx_buffer[FINAL_RESP_RX_TS_IDX], &resp_rx_ts_i);
        ts_get_32(&rx_buffer[FINAL_TX_TS_IDX],      &final_tx_ts_i);

        /* DS-TWR distance formula (32-bit truncation handles clock wrap) */
        uint32_t poll_rx_ts_32  = (uint32_t)poll_rx_ts;
        uint32_t resp_tx_ts_32  = (uint32_t)resp_tx_ts;
        uint32_t final_rx_ts_32 = (uint32_t)final_rx_ts;

        double Ra = (double)(resp_rx_ts_i  - poll_tx_ts_i);
        double Rb = (double)(final_rx_ts_32 - resp_tx_ts_32);
        double Da = (double)(final_tx_ts_i  - resp_rx_ts_i);
        double Db = (double)(resp_tx_ts_32  - poll_rx_ts_32);

        double tof_dtu = ((Ra * Rb) - (Da * Db)) / (Ra + Rb + Da + Db);
        double tof_s   = tof_dtu * DWT_TIME_UNITS;
        float  dist_m  = (float)(tof_s * SPEED_OF_LIGHT);

        LOG_INF("Anchor 0x%04X <-> Tag 0x%04X : %.3f m",
                my_addr, tag_addr, (double)dist_m);

        if (distance_cb) {
            distance_cb(my_addr, tag_addr, dist_m);
        }
    }
}

/* ── DS-TWR Tag (Initiator) Logic ─────────────────────────────────── */
static void tag_ranging_loop(void)
{
    LOG_INF("UWB Tag 0x%04X: Starting anchor sweep", my_addr);

    /* Set up RX-after-TX delay and timeout for RESP waiting */
    dwt_setrxaftertxdelay(POLL_TX_TO_RESP_RX_DLY_UUS);
    dwt_setrxtimeout(RESP_RX_TIMEOUT_UUS);
    dwt_setpreambledetecttimeout(PRE_TIMEOUT);

    while (1) {
        for (int i = 0; i < UWB_MAX_ANCHORS; i++) {
            uint16_t anchor_addr = anchor_list[i];
            if (anchor_addr == 0x0000) {
                break; /* End of list sentinel */
            }

            uint32_t status_reg;

            /* Send POLL to this anchor */
            tx_poll_msg[SEQ_IDX] = frame_seq_nb;
            frame_set_addr(tx_poll_msg, ADDR_DST_IDX, anchor_addr);
            frame_set_addr(tx_poll_msg, ADDR_SRC_IDX, my_addr);

            dwt_writetxdata(sizeof(tx_poll_msg), tx_poll_msg, 0);
            dwt_writetxfctrl(sizeof(tx_poll_msg) + FCS_LEN, 0, 1);
            dwt_starttx(DWT_START_TX_IMMEDIATE | DWT_RESPONSE_EXPECTED);

            wait_for_status(&status_reg,
                DWT_INT_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
            frame_seq_nb++;

            if (!(status_reg & DWT_INT_RXFCG_BIT_MASK)) {
                /* No RESP received from this anchor (out of range / busy) */
                dwt_writesysstatuslo(SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR
                                     | DWT_INT_TXFRS_BIT_MASK);
                LOG_DBG("No RESP from anchor 0x%04X", anchor_addr);
                continue;
            }

            dwt_writesysstatuslo(DWT_INT_RXFCG_BIT_MASK | DWT_INT_TXFRS_BIT_MASK);

            uint16_t frame_len = dwt_getframelength(NULL);
            if (frame_len > RX_BUF_LEN) {
                continue;
            }
            dwt_readrxdata(rx_buffer, frame_len, 0);

            /* Validate RESP: from expected anchor, to us, function=RESP */
            uint16_t dst = frame_get_addr(rx_buffer, ADDR_DST_IDX);
            uint16_t src = frame_get_addr(rx_buffer, ADDR_SRC_IDX);
            if (dst != my_addr || src != anchor_addr ||
                rx_buffer[FUNC_IDX] != UWB_FC_RESP) {
                continue;
            }

            /* Read timestamps and schedule delayed FINAL */
            uint64_t poll_tx_ts = get_tx_timestamp_u64();
            uint64_t resp_rx_ts = get_rx_timestamp_u64();

            uint32_t final_tx_time =
                (resp_rx_ts + (RESP_RX_TO_FINAL_TX_DLY_UUS * UUS_TO_DWT_TIME)) >> 8;
            dwt_setdelayedtrxtime(final_tx_time);

            /* Predicted final TX timestamp (for embedding in FINAL frame) */
            uint64_t final_tx_ts =
                (((uint64_t)(final_tx_time & 0xFFFFFFFEUL)) << 8)
                + CONFIG_UWB_TX_ANTENNA_DELAY;

            /* Build FINAL frame with all three timestamps */
            tx_final_msg[SEQ_IDX] = frame_seq_nb;
            frame_set_addr(tx_final_msg, ADDR_DST_IDX, anchor_addr);
            frame_set_addr(tx_final_msg, ADDR_SRC_IDX, my_addr);
            ts_set_32(&tx_final_msg[FINAL_POLL_TX_TS_IDX], poll_tx_ts);
            ts_set_32(&tx_final_msg[FINAL_RESP_RX_TS_IDX], resp_rx_ts);
            ts_set_32(&tx_final_msg[FINAL_TX_TS_IDX],      final_tx_ts);

            dwt_writetxdata(sizeof(tx_final_msg), tx_final_msg, 0);
            dwt_writetxfctrl(sizeof(tx_final_msg) + FCS_LEN, 0, 1);

            int ret = dwt_starttx(DWT_START_TX_DELAYED);
            if (ret == DWT_SUCCESS) {
                wait_for_status(NULL, DWT_INT_TXFRS_BIT_MASK);
                dwt_writesysstatuslo(DWT_INT_TXFRS_BIT_MASK);
                frame_seq_nb++;
                LOG_DBG("FINAL sent to anchor 0x%04X", anchor_addr);
            } else {
                LOG_WRN("FINAL TX late for anchor 0x%04X", anchor_addr);
                dwt_writesysstatuslo(DWT_INT_TXFRS_BIT_MASK | SYS_STATUS_ALL_RX_ERR);
            }

            /* Small inter-anchor gap to avoid collisions */
            k_msleep(50);
        }

        /* Wait between full ranging sweeps */
        k_msleep(CONFIG_UWB_RANGING_INTERVAL_MS);
    }
}

/* ── UWB Thread Entry Point ──────────────────────────────────────── */
static void uwb_thread_entry(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    if (IS_ENABLED(CONFIG_NODE_ROLE_ANCHOR)) {
        anchor_ranging_loop();
    } else {
        tag_ranging_loop();
    }
}

/* ── Public API ───────────────────────────────────────────────────── */

int uwb_manager_init(void)
{
    int ret;

    my_addr = CONFIG_UWB_NODE_SHORT_ADDR;

    /* Patch PAN ID from config */
    uint16_t pan_id = CONFIG_UWB_PAN_ID;
    tx_poll_msg[3]  = tx_resp_msg[3]  = tx_final_msg[3]  = (uint8_t)(pan_id & 0xFF);
    tx_poll_msg[4]  = tx_resp_msg[4]  = tx_final_msg[4]  = (uint8_t)(pan_id >> 8);

    /* Hardware init: SPI + GPIO */
    ret = dw3000_hw_init();
    if (ret < 0) {
        LOG_ERR("dw3000_hw_init failed: %d", ret);
        return ret;
    }

    /* Configure SPI fast rate */
    dw3000_spi_speed_fast();

    /* Hardware reset */
    dw3000_hw_reset();
    k_msleep(2);

    /* Probe for DW3000 device driver */
    dwt_probe((struct dwt_probe_s *)&dw3000_probe_interf);

    /* Wait for IDLE_RC state */
    uint32_t timeout = 1000;
    while (!dwt_checkidlerc() && timeout--) {
        k_msleep(1);
    }
    if (timeout == 0) {
        LOG_ERR("DW3000 did not reach IDLE_RC");
        return -ETIMEDOUT;
    }

    /* Initialize device */
    if (dwt_initialise(DWT_DW_INIT) == DWT_ERROR) {
        LOG_ERR("dwt_initialise failed");
        return -EIO;
    }

    /* Apply RF configuration */
    if (dwt_configure(&uwb_config) == DWT_ERROR) {
        LOG_ERR("dwt_configure failed (PLL or RX calibration)");
        return -EIO;
    }

    /* Apply TX power / spectrum config */
    dwt_configuretxrf(&txconfig);

    /* Set antenna delays */
    dwt_setrxantennadelay(CONFIG_UWB_RX_ANTENNA_DELAY);
    dwt_settxantennadelay(CONFIG_UWB_TX_ANTENNA_DELAY);

    /* Enable LNA/PA for better range */
    dwt_setlnapamode(DWT_LNA_ENABLE | DWT_PA_ENABLE);

    LOG_INF("DW3000 initialized. Node addr=0x%04X role=%s",
            my_addr,
            IS_ENABLED(CONFIG_NODE_ROLE_ANCHOR) ? "ANCHOR" : "TAG");

    return 0;
}

void uwb_manager_set_distance_cb(uwb_distance_cb_t cb)
{
    distance_cb = cb;
}

int uwb_manager_start(void)
{
    k_thread_create(&uwb_thread_data, uwb_thread_stack,
                    K_THREAD_STACK_SIZEOF(uwb_thread_stack),
                    uwb_thread_entry, NULL, NULL, NULL,
                    UWB_THREAD_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&uwb_thread_data, "uwb_ranging");
    return 0;
}
