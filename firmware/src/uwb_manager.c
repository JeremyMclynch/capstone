/*
 * UWB Manager - DS-TWR Ranging (interrupt-driven)
 *
 * Architecture:
 *   - DW3000 IRQ pin → GPIO interrupt → k_work → dwt_isr() → callbacks
 *   - Callbacks post a k_sem to wake the UWB thread
 *   - UWB thread blocks on k_sem between events (no spin-loops)
 *   - Critical timing sections (POLL_RX→RESP_TX, RESP_RX→FINAL_TX) still
 *     use k_sched_lock() to prevent preemption during the delay calculation
 *     and dwt_starttx() call
 *
 * Role selected at compile time:
 *   CONFIG_NODE_ROLE_ANCHOR=y  -> responder  (receives POLL, sends RESP,
 *                                              receives FINAL, computes distance)
 *   CONFIG_NODE_ROLE_TAG=y     -> initiator  (sends POLL, receives RESP,
 *                                              sends FINAL)
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <string.h>
#include <math.h>

#include "deca_probe_interface.h"
#include "deca_device_api.h"
#include "dw3000_hw.h"
#include "dw3000_spi.h"

#include "uwb_manager.h"
#include "thread_coap.h"
#include "device_config.h"

LOG_MODULE_REGISTER(uwb_manager, LOG_LEVEL_INF);

/* ── PHY config ──────────────────────────────────────────────────── */
static dwt_config_t uwb_config = {
    5,               /* Channel 5 */
    DWT_PLEN_128,
    DWT_PAC8,
    9,               /* TX preamble code */
    9,               /* RX preamble code */
    1,               /* Non-standard 8-symbol SFD */
    DWT_BR_6M8,
    DWT_PHRMODE_STD,
    DWT_PHRRATE_STD,
    (129 + 8 - 8),   /* SFD timeout */
    DWT_STS_MODE_OFF,
    DWT_STS_LEN_64,
    DWT_PDOA_M0
};

static dwt_txconfig_t txconfig = {
    0x34,
    0xfdfdfdfd,
    0x0
};

/* ── Antenna delays ──────────────────────────────────────────────── */
#define OTP_CH5_ANTDELAY_ADDR 0x1A
#define ANT_DLY_FALLBACK      16366  /* Qorvo characterized value for DWM3001CDK/DWM3000EVB */

/* ── Timing constants ────────────────────────────────────────────── */
#define POLL_RX_TO_RESP_TX_DLY_UUS  3500
#define RESP_TX_TO_FINAL_RX_DLY_UUS 1000
#define FINAL_RX_TIMEOUT_UUS        5000
#define PRE_TIMEOUT                   64
#define POLL_TX_TO_RESP_RX_DLY_UUS  (POLL_RX_TO_RESP_TX_DLY_UUS - 200)
#define RESP_RX_TO_FINAL_TX_DLY_UUS 3500
#define RESP_RX_TIMEOUT_UUS         1000

#define UUS_TO_DWT_TIME  65536
#define SPEED_OF_LIGHT   299702547.0

/* ── Runtime state ───────────────────────────────────────────────── */
static atomic_t uwb_running = ATOMIC_INIT(0);
static bool     uwb_started = false;       /* thread created? */
static uint16_t ranging_interval_ms = 200;  /* overwritten from g_config */
static uint32_t last_distance_mm;
static uint32_t range_count;
static uint16_t tx_ant_dly;                 /* resolved from OTP at init */

/* ── Frame definitions ───────────────────────────────────────────── */
#define ALL_MSG_COMMON_LEN        10
#define ALL_MSG_SN_IDX             2
#define FINAL_MSG_POLL_TX_TS_IDX  10
#define FINAL_MSG_RESP_RX_TS_IDX  14
#define FINAL_MSG_FINAL_TX_TS_IDX 18

/* Initiator (tag) frames */
static uint8_t tx_poll_msg[]  = { 0x41, 0x88, 0, 0xCA, 0xDE,
                                   'W','A','V','E', 0x21, 0, 0 };
static uint8_t rx_resp_msg[]  = { 0x41, 0x88, 0, 0xCA, 0xDE,
                                   'V','E','W','A', 0x10, 0x02, 0, 0 };
static uint8_t tx_final_msg[] = { 0x41, 0x88, 0, 0xCA, 0xDE,
                                   'W','A','V','E', 0x23,
                                   0,0,0,0, 0,0,0,0, 0,0,0,0 };

/* Responder (anchor) frames */
static uint8_t rx_poll_msg[]  = { 0x41, 0x88, 0, 0xCA, 0xDE,
                                   'W','A','V','E', 0x21, 0, 0 };
static uint8_t tx_resp_msg[]  = { 0x41, 0x88, 0, 0xCA, 0xDE,
                                   'V','E','W','A', 0x10, 0x02, 0, 0, 0, 0 };
static uint8_t rx_final_msg[] = { 0x41, 0x88, 0, 0xCA, 0xDE,
                                   'W','A','V','E', 0x23,
                                   0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0 };

#define RX_BUF_LEN 24
static uint8_t rx_buffer[RX_BUF_LEN];

static uint8_t frame_seq_nb = 0;

/* ── IRQ event signalling ─────────────────────────────────────────── */
static K_SEM_DEFINE(sem_tx_done, 0, 1);
static K_SEM_DEFINE(sem_rx_ok,   0, 1);
static K_SEM_DEFINE(sem_rx_to,   0, 1);
static K_SEM_DEFINE(sem_rx_err,  0, 1);

typedef enum {
    EVT_TX_DONE,
    EVT_RX_OK,
    EVT_RX_TO,
    EVT_RX_ERR,
} uwb_event_t;

/* ── DW3000 callbacks ────────────────────────────────────────────── */
static void cb_tx_done(const dwt_cb_data_t *data) { ARG_UNUSED(data); k_sem_give(&sem_tx_done); }
static void cb_rx_ok(const dwt_cb_data_t *data)   { ARG_UNUSED(data); k_sem_give(&sem_rx_ok); }
static void cb_rx_to(const dwt_cb_data_t *data)   { ARG_UNUSED(data); k_sem_give(&sem_rx_to); }
static void cb_rx_err(const dwt_cb_data_t *data)  { ARG_UNUSED(data); k_sem_give(&sem_rx_err); }

static uwb_event_t wait_for_event(k_timeout_t timeout)
{
    bool forever = K_TIMEOUT_EQ(timeout, K_FOREVER);
    int64_t deadline = forever ? 0 :
        k_uptime_get() + k_ticks_to_ms_floor64(timeout.ticks);

    while (1) {
        if (k_sem_take(&sem_rx_ok,   K_NO_WAIT) == 0) return EVT_RX_OK;
        if (k_sem_take(&sem_tx_done, K_NO_WAIT) == 0) return EVT_TX_DONE;
        if (k_sem_take(&sem_rx_to,   K_NO_WAIT) == 0) return EVT_RX_TO;
        if (k_sem_take(&sem_rx_err,  K_NO_WAIT) == 0) return EVT_RX_ERR;

        if (!forever && k_uptime_get() >= deadline) return EVT_RX_TO;

        k_sleep(K_USEC(100));
    }
}

/* ── Timestamp helpers ───────────────────────────────────────────── */
static uint64_t get_tx_timestamp_u64(void)
{
    uint8_t ts[5];
    dwt_readtxtimestamp(ts);
    uint64_t t = 0;
    for (int i = 4; i >= 0; i--) t = (t << 8) | ts[i];
    return t;
}

static uint64_t get_rx_timestamp_u64(void)
{
    uint8_t ts[5];
    dwt_readrxtimestamp(ts, DWT_IP_M);
    uint64_t t = 0;
    for (int i = 4; i >= 0; i--) t = (t << 8) | ts[i];
    return t;
}

static void final_msg_set_ts(uint8_t *ts_field, uint64_t ts)
{
    for (int i = 0; i < 4; i++)
        ts_field[i] = (uint8_t)(ts >> (i * 8));
}

static void final_msg_get_ts(const uint8_t *ts_field, uint32_t *ts)
{
    *ts = 0;
    for (int i = 0; i < 4; i++)
        *ts |= (uint32_t)ts_field[i] << (i * 8);
}

/* ── UWB thread ───────────────────────────────────────────────────── */
#define UWB_STACK_SIZE 2048
#define UWB_PRIORITY   0

K_THREAD_STACK_DEFINE(uwb_stack, UWB_STACK_SIZE);
static struct k_thread uwb_thread_data;

static uwb_distance_cb_t distance_cb;

/* Drain all stale semaphores so a stop→start transition doesn't
 * see leftover events from a previous cycle. */
static void drain_semaphores(void)
{
    while (k_sem_take(&sem_tx_done, K_NO_WAIT) == 0) {}
    while (k_sem_take(&sem_rx_ok,   K_NO_WAIT) == 0) {}
    while (k_sem_take(&sem_rx_to,   K_NO_WAIT) == 0) {}
    while (k_sem_take(&sem_rx_err,  K_NO_WAIT) == 0) {}
}

/* ── Responder (anchor) ──────────────────────────────────────────── */
static void responder_loop(void)
{
    LOG_INF("DS-TWR Responder ready (interrupt-driven)");

    bool was_stopped = false;

    while (1) {
        /* Check if we should be idle */
        if (!atomic_get(&uwb_running)) {
            if (!was_stopped) {
                dwt_forcetrxoff();
                was_stopped = true;
            }
            k_sleep(K_MSEC(100));
            continue;
        }

        /* Re-entering active mode after a stop */
        if (was_stopped) {
            drain_semaphores();
            dwt_forcetrxoff();
            dwt_writesysstatuslo(SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR |
                                 DWT_INT_RXFCG_BIT_MASK | DWT_INT_TXFRS_BIT_MASK);
            was_stopped = false;
            LOG_INF("Responder resuming");
        }

        /* ── Wait for POLL ── */
        dwt_setpreambledetecttimeout(0);
        dwt_setrxtimeout(0);
        dwt_rxenable(DWT_START_RX_IMMEDIATE);

        uwb_event_t evt = wait_for_event(K_FOREVER);
        if (!atomic_get(&uwb_running)) continue;

        if (evt != EVT_RX_OK) {
            dwt_writesysstatuslo(SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
            continue;
        }

        uint16_t frame_len = dwt_getframelength(NULL);
        if (frame_len <= RX_BUF_LEN)
            dwt_readrxdata(rx_buffer, frame_len, 0);

        rx_buffer[ALL_MSG_SN_IDX] = 0;
        if (memcmp(rx_buffer, rx_poll_msg, ALL_MSG_COMMON_LEN) != 0)
            continue;

        /* Extract tag source address from POLL bytes 10-11 (LE) */
        uint16_t tag_addr = (uint16_t)rx_buffer[10] | ((uint16_t)rx_buffer[11] << 8);

        k_sched_lock();
        uint64_t poll_rx_ts = get_rx_timestamp_u64();
        uint32_t resp_tx_time =
            (poll_rx_ts + (POLL_RX_TO_RESP_TX_DLY_UUS * UUS_TO_DWT_TIME)) >> 8;
        dwt_setdelayedtrxtime(resp_tx_time);
        dwt_setrxaftertxdelay(RESP_TX_TO_FINAL_RX_DLY_UUS);
        dwt_setrxtimeout(FINAL_RX_TIMEOUT_UUS);
        dwt_setpreambledetecttimeout(0);

        tx_resp_msg[ALL_MSG_SN_IDX] = frame_seq_nb;
        dwt_writetxdata(sizeof(tx_resp_msg), tx_resp_msg, 0);
        dwt_writetxfctrl(sizeof(tx_resp_msg), 0, 1);
        int tx_ret = dwt_starttx(DWT_START_TX_DELAYED | DWT_RESPONSE_EXPECTED);
        k_sched_unlock();

        if (tx_ret == DWT_ERROR) {
            LOG_WRN("RESP TX too late");
            continue;
        }

        evt = wait_for_event(K_MSEC(10));
        if (evt != EVT_TX_DONE) {
            LOG_WRN("RESP TX confirmation lost");
            dwt_forcetrxoff();
            continue;
        }

        evt = wait_for_event(K_MSEC(20));
        frame_seq_nb++;

        if (evt != EVT_RX_OK) {
            dwt_writesysstatuslo(SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
            LOG_WRN("No FINAL received");
            continue;
        }

        dwt_writesysstatuslo(DWT_INT_RXFCG_BIT_MASK | DWT_INT_TXFRS_BIT_MASK);

        frame_len = dwt_getframelength(NULL);
        if (frame_len <= RX_BUF_LEN)
            dwt_readrxdata(rx_buffer, frame_len, 0);

        rx_buffer[ALL_MSG_SN_IDX] = 0;
        if (memcmp(rx_buffer, rx_final_msg, ALL_MSG_COMMON_LEN) != 0)
            continue;

        /* ── Compute distance ── */
        uint64_t resp_tx_ts  = get_tx_timestamp_u64();
        uint64_t final_rx_ts = get_rx_timestamp_u64();

        uint32_t poll_tx_ts_i, resp_rx_ts_i, final_tx_ts_i;
        final_msg_get_ts(&rx_buffer[FINAL_MSG_POLL_TX_TS_IDX],  &poll_tx_ts_i);
        final_msg_get_ts(&rx_buffer[FINAL_MSG_RESP_RX_TS_IDX],  &resp_rx_ts_i);
        final_msg_get_ts(&rx_buffer[FINAL_MSG_FINAL_TX_TS_IDX], &final_tx_ts_i);

        uint32_t poll_rx_ts_32  = (uint32_t)poll_rx_ts;
        uint32_t resp_tx_ts_32  = (uint32_t)resp_tx_ts;
        uint32_t final_rx_ts_32 = (uint32_t)final_rx_ts;

        double Ra = (double)(resp_rx_ts_i  - poll_tx_ts_i);
        double Rb = (double)(final_rx_ts_32 - resp_tx_ts_32);
        double Da = (double)(final_tx_ts_i  - resp_rx_ts_i);
        double Db = (double)(resp_tx_ts_32  - poll_rx_ts_32);

        double tof_dtu = (Ra * Rb - Da * Db) / (Ra + Rb + Da + Db);
        double tof_s   = tof_dtu * DWT_TIME_UNITS;
        float  dist_m  = (float)(tof_s * SPEED_OF_LIGHT);

        last_distance_mm = (uint32_t)(dist_m * 1000.0f);

        /* Apply calibration offset */
        int32_t calibrated_mm = (int32_t)last_distance_mm + g_config.calibration_offset_mm;
        last_distance_mm = (calibrated_mm > 0) ? (uint32_t)calibrated_mm : 0;

        float calibrated_dist = dist_m + (g_config.calibration_offset_mm / 1000.0f);
        if (calibrated_dist < 0.0f) calibrated_dist = 0.0f;

        range_count++;

        /* Read per-packet signal quality diagnostics from FINAL RX */
        dwt_cirdiags_t cir_diag;
        int16_t rssi_q8 = 0, fp_power_q8 = 0;
        dwt_readdiagnostics_acc(&cir_diag, DWT_ACC_IDX_IP_M);
        dwt_calculate_rssi(&cir_diag, DWT_ACC_IDX_IP_M, &rssi_q8);
        dwt_calculate_first_path_power(&cir_diag, DWT_ACC_IDX_IP_M, &fp_power_q8);

        LOG_INF("Distance: %.3f m (tag=0x%04X) rssi=%.1f fp=%.1f",
                (double)calibrated_dist, tag_addr,
                (double)rssi_q8 / 256.0, (double)fp_power_q8 / 256.0);

        if (distance_cb)
            distance_cb(g_config.uwb_addr, tag_addr, calibrated_dist,
                        rssi_q8, fp_power_q8, cir_diag.FpIndex, cir_diag.peakIndex);
    }
}

/* ── Initiator (tag) ─────────────────────────────────────────────── */
static void initiator_loop(void)
{
    LOG_INF("DS-TWR Initiator ready (interrupt-driven)");
    bool was_stopped = false;

    while (1) {
        /* Check if we should be idle */
        if (!atomic_get(&uwb_running)) {
            if (!was_stopped) {
                dwt_forcetrxoff();
                was_stopped = true;
            }
            k_sleep(K_MSEC(100));
            continue;
        }

        if (was_stopped) {
            drain_semaphores();
            dwt_forcetrxoff();
            dwt_writesysstatuslo(SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR |
                                 DWT_INT_RXFCG_BIT_MASK | DWT_INT_TXFRS_BIT_MASK);
            was_stopped = false;
            LOG_INF("Initiator resuming");
        }

        /* ── Send POLL, arm RX for RESP ── */
        dwt_setrxaftertxdelay(POLL_TX_TO_RESP_RX_DLY_UUS);
        dwt_setrxtimeout(RESP_RX_TIMEOUT_UUS);
        dwt_setpreambledetecttimeout(PRE_TIMEOUT);

        LOG_INF("[TAG] Sending POLL (seq=%u)", frame_seq_nb);
        thread_coap_send_event(g_config.uwb_addr, UWB_EVT_POLL_TX, frame_seq_nb);
        tx_poll_msg[ALL_MSG_SN_IDX] = frame_seq_nb;
        tx_poll_msg[10] = (uint8_t)(g_config.uwb_addr & 0xFF);
        tx_poll_msg[11] = (uint8_t)(g_config.uwb_addr >> 8);
        dwt_writetxdata(sizeof(tx_poll_msg), tx_poll_msg, 0);
        dwt_writetxfctrl(sizeof(tx_poll_msg) + FCS_LEN, 0, 1);
        dwt_starttx(DWT_START_TX_IMMEDIATE | DWT_RESPONSE_EXPECTED);

        /* Wait for TX done (POLL sent) */
        uwb_event_t evt = wait_for_event(K_MSEC(10));
        if (evt != EVT_TX_DONE) {
            LOG_WRN("POLL TX confirmation lost");
            dwt_forcetrxoff();
            k_sleep(K_MSEC(ranging_interval_ms));
            continue;
        }
        LOG_INF("[TAG] POLL sent, waiting for RESP...");

        /* Wait for RESP */
        evt = wait_for_event(K_MSEC(20));
        frame_seq_nb++;

        if (evt != EVT_RX_OK) {
            dwt_writesysstatuslo(SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR |
                                 DWT_INT_TXFRS_BIT_MASK);
            LOG_INF("[TAG] No RESP received");
            thread_coap_send_event(g_config.uwb_addr, UWB_EVT_NO_RESP, frame_seq_nb);
            k_sleep(K_MSEC(ranging_interval_ms));
            continue;
        }

        LOG_INF("[TAG] RESP received");
        thread_coap_send_event(g_config.uwb_addr, UWB_EVT_RESP_RX, frame_seq_nb);
        dwt_writesysstatuslo(DWT_INT_RXFCG_BIT_MASK | DWT_INT_TXFRS_BIT_MASK);

        uint16_t frame_len = dwt_getframelength(NULL);
        if (frame_len <= RX_BUF_LEN)
            dwt_readrxdata(rx_buffer, frame_len, 0);

        rx_buffer[ALL_MSG_SN_IDX] = 0;
        if (memcmp(rx_buffer, rx_resp_msg, ALL_MSG_COMMON_LEN) != 0) {
            k_sleep(K_MSEC(ranging_interval_ms));
            continue;
        }

        /* ── Send FINAL ── */
        k_sched_lock();
        uint64_t poll_tx_ts = get_tx_timestamp_u64();
        uint64_t resp_rx_ts = get_rx_timestamp_u64();

        uint32_t final_tx_time =
            (resp_rx_ts + (RESP_RX_TO_FINAL_TX_DLY_UUS * UUS_TO_DWT_TIME)) >> 8;
        dwt_setdelayedtrxtime(final_tx_time);

        uint64_t final_tx_ts =
            (((uint64_t)(final_tx_time & 0xFFFFFFFEUL)) << 8) + tx_ant_dly;

        final_msg_set_ts(&tx_final_msg[FINAL_MSG_POLL_TX_TS_IDX],  poll_tx_ts);
        final_msg_set_ts(&tx_final_msg[FINAL_MSG_RESP_RX_TS_IDX],  resp_rx_ts);
        final_msg_set_ts(&tx_final_msg[FINAL_MSG_FINAL_TX_TS_IDX], final_tx_ts);

        LOG_INF("[TAG] Sending FINAL (seq=%u)", frame_seq_nb);
        thread_coap_send_event(g_config.uwb_addr, UWB_EVT_FINAL_TX, frame_seq_nb);
        tx_final_msg[ALL_MSG_SN_IDX] = frame_seq_nb;
        dwt_writetxdata(sizeof(tx_final_msg), tx_final_msg, 0);
        dwt_writetxfctrl(sizeof(tx_final_msg) + FCS_LEN, 0, 1);
        int final_ret = dwt_starttx(DWT_START_TX_DELAYED);
        k_sched_unlock();

        if (final_ret == DWT_SUCCESS) {
            evt = wait_for_event(K_MSEC(10));
            if (evt == EVT_TX_DONE) {
                LOG_INF("[TAG] FINAL sent — cycle complete (seq=%u)", frame_seq_nb);
                range_count++;
                frame_seq_nb++;
            } else {
                LOG_WRN("FINAL TX confirmation lost");
                dwt_forcetrxoff();
            }
        } else {
            LOG_WRN("FINAL TX too late");
            dwt_writesysstatuslo(DWT_INT_TXFRS_BIT_MASK | SYS_STATUS_ALL_RX_ERR);
        }

        k_sleep(K_MSEC(ranging_interval_ms));
    }
}

/* ── Thread entry ─────────────────────────────────────────────────── */
static void uwb_thread_entry(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
    if (g_config.role == ROLE_ANCHOR)
        responder_loop();
    else
        initiator_loop();
}

/* ── Public API ───────────────────────────────────────────────────── */
int uwb_manager_init(void)
{
    /* Load interval from runtime config */
    ranging_interval_ms = g_config.ranging_interval_ms;

    int ret = dw3000_hw_init();
    if (ret < 0) {
        LOG_ERR("dw3000_hw_init failed: %d", ret);
        return ret;
    }

    dw3000_hw_reset();
    k_msleep(2);

    if (dwt_probe((struct dwt_probe_s *)&dw3000_probe_interf) != DWT_SUCCESS) {
        LOG_ERR("dwt_probe failed");
        return -EIO;
    }
    LOG_INF("dwt_probe OK");

    /* Switch to fast SPI after successful probe (probe runs at 2MHz slow speed) */
    dw3000_spi_speed_fast();

    /*
     * dwt_initialise() must be called before dwt_checkidlerc() because
     * ull_initialise() assigns dw->priv which all register-access functions
     * (via dwt_xfer3xxx / LOCAL_DATA(dw)->spicrc) depend on.
     */
    if (dwt_initialise(DWT_DW_INIT) == DWT_ERROR) {
        LOG_ERR("dwt_initialise failed");
        return -EIO;
    }

    uint32_t timeout = 200;
    while (!dwt_checkidlerc() && timeout--)
        k_msleep(1);
    if (!timeout) {
        LOG_ERR("DW3000 not idle");
        return -ETIMEDOUT;
    }

    if (dwt_configure(&uwb_config) == DWT_ERROR) {
        LOG_ERR("dwt_configure failed");
        return -EIO;
    }

    dwt_configuretxrf(&txconfig);
    dwt_configciadiag(DW_CIA_DIAG_LOG_ALL);

    /* Read factory-calibrated antenna delay from OTP (Channel 5) */
    uint32_t otp_ant_dly;
    dwt_otpread(OTP_CH5_ANTDELAY_ADDR, &otp_ant_dly, 1);

    tx_ant_dly = otp_ant_dly & 0xFFFF;
    uint16_t rx_ant_dly = (otp_ant_dly >> 16) & 0xFFFF;

    /* Sanity-check: valid antenna delays are ~16000-16500 for DW3000.
     * Values outside this range indicate blank/corrupt OTP → use fallback. */
    if (tx_ant_dly < 10000 || tx_ant_dly > 20000) tx_ant_dly = ANT_DLY_FALLBACK;
    if (rx_ant_dly < 10000 || rx_ant_dly > 20000) rx_ant_dly = ANT_DLY_FALLBACK;

    LOG_INF("Antenna delay: TX=%u RX=%u (OTP=0x%08X)", tx_ant_dly, rx_ant_dly, otp_ant_dly);
    dwt_setrxantennadelay(rx_ant_dly);
    dwt_settxantennadelay(tx_ant_dly);
    dwt_setlnapamode(DWT_LNA_ENABLE | DWT_PA_ENABLE);
    dwt_setleds(DWT_LEDS_ENABLE | DWT_LEDS_INIT_BLINK);

    static dwt_callbacks_s cbs = {
        .cbTxDone = cb_tx_done,
        .cbRxOk   = cb_rx_ok,
        .cbRxTo   = cb_rx_to,
        .cbRxErr  = cb_rx_err,
    };
    dwt_setcallbacks(&cbs);

    dwt_setinterrupt(
        DWT_INT_TXFRS_BIT_MASK |
        DWT_INT_RXFCG_BIT_MASK |
        DWT_INT_RXFTO_BIT_MASK |
        DWT_INT_RXPTO_BIT_MASK |
        DWT_INT_RXPHE_BIT_MASK |
        DWT_INT_RXFCE_BIT_MASK |
        DWT_INT_RXFSL_BIT_MASK |
        DWT_INT_RXSTO_BIT_MASK,
        0,
        DWT_ENABLE_INT_ONLY);

    ret = dw3000_hw_init_interrupt();
    if (ret < 0) {
        LOG_ERR("IRQ init failed: %d", ret);
        return ret;
    }

    LOG_INF("DW3000 ready (IRQ). Role: %s",
            g_config.role == ROLE_ANCHOR ? "RESPONDER" : "INITIATOR");
    return 0;
}

void uwb_manager_set_distance_cb(uwb_distance_cb_t cb)
{
    distance_cb = cb;
}

int uwb_manager_start(void)
{
    if (atomic_get(&uwb_running)) {
        return -EALREADY;
    }

    atomic_set(&uwb_running, 1);

    if (!uwb_started) {
        k_thread_create(&uwb_thread_data, uwb_stack,
                        K_THREAD_STACK_SIZEOF(uwb_stack),
                        uwb_thread_entry, NULL, NULL, NULL,
                        UWB_PRIORITY, 0, K_NO_WAIT);
        k_thread_name_set(&uwb_thread_data, "uwb");
        uwb_started = true;
    }

    LOG_INF("UWB ranging started");
    return 0;
}

void uwb_manager_stop(void)
{
    atomic_set(&uwb_running, 0);
    /* The thread loop will call dwt_forcetrxoff() on next iteration */
    LOG_INF("UWB ranging stopped");
}

void uwb_manager_set_interval(uint16_t interval_ms)
{
    ranging_interval_ms = interval_ms;
}

void uwb_manager_get_status(struct uwb_status *status)
{
    status->running          = atomic_get(&uwb_running) != 0;
    status->last_distance_mm = last_distance_mm;
    status->range_count      = range_count;
}

uint32_t uwb_manager_get_last_distance_mm(void)
{
    return last_distance_mm;
}
