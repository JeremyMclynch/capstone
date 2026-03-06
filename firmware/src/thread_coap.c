/*
 * Thread CoAP Client - Distance Reporting
 *
 * Manages the OpenThread network connection and sends distance
 * measurement reports to the CoAP server as UDP POST requests.
 *
 * CoAP resource: POST /distance
 *
 * Binary payload (20 bytes, little-endian):
 *   [0-1]   anchor_id    (uint16_t)
 *   [2-3]   tag_id       (uint16_t)
 *   [4-7]   distance_mm  (uint32_t, millimeters)
 *   [8-11]  uptime_s     (uint32_t, seconds since boot)
 *   [12-13] rssi_q8      (int16_t, channel RSSI in Q8.8 dBm)
 *   [14-15] fp_power_q8  (int16_t, first-path power in Q8.8 dBm)
 *   [16-17] fp_index     (uint16_t, first-path CIR index, Q10.6)
 *   [18-19] peak_index   (uint16_t, peak CIR sample index)
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/coap.h>
#include <net/coap_utils.h>
#include <openthread.h>
#include <openthread/thread.h>

#include "thread_coap.h"
#include "device_config.h"

LOG_MODULE_REGISTER(thread_coap, LOG_LEVEL_INF);

/* CoAP resource path components */
static const char *const distance_uri[] = { "distance", NULL };
static const char *const event_uri[]    = { "event",    NULL };
static const char *const cir_uri[]      = { "cir",      NULL };

/* Server address (parsed from CONFIG_COAP_SERVER_ADDR at init) */
static struct sockaddr_in6 server_addr = {
    .sin6_family = AF_INET6,
    .sin6_port   = 0,   /* filled at init */
};

/* Work queue for async CoAP sends */
#define COAP_WORKQ_STACK_SIZE 2048
#define COAP_WORKQ_PRIORITY   5

K_THREAD_STACK_DEFINE(coap_workq_stack, COAP_WORKQ_STACK_SIZE);
static struct k_work_q coap_workq;

/* Pending measurement for the work item */
struct distance_measurement {
    uint16_t anchor_id;
    uint16_t tag_id;
    float    distance_m;
    int16_t  rssi_q8;
    int16_t  fp_power_q8;
    uint16_t fp_index;
    uint16_t peak_index;
};

#define MEAS_QUEUE_DEPTH 8
K_MSGQ_DEFINE(meas_queue, sizeof(struct distance_measurement),
              MEAS_QUEUE_DEPTH, 4);

/* Pending UWB event for the event work item */
struct uwb_event_msg {
    uint16_t node_id;
    uint8_t  event;
    uint8_t  seq;
};

#define EVT_QUEUE_DEPTH 16
K_MSGQ_DEFINE(evt_queue, sizeof(struct uwb_event_msg),
              EVT_QUEUE_DEPTH, 4);

/* CIR capture payload: 20-byte header + 192 bytes of I/Q data */
#define CIR_NUM_SAMPLES 48

struct cir_measurement {
    uint16_t anchor_id;
    uint16_t tag_id;
    uint32_t distance_mm;
    uint16_t fp_index;
    uint16_t sample_offset;
    uint8_t  num_samples;
    uint32_t cir_data[CIR_NUM_SAMPLES];
};

#define CIR_QUEUE_DEPTH 2
K_MSGQ_DEFINE(cir_queue, sizeof(struct cir_measurement),
              CIR_QUEUE_DEPTH, 4);

static struct k_work send_work;
static struct k_work send_evt_work;
static struct k_work send_cir_work;
static bool thread_connected = false;

/* ── Binary payload encoding ─────────────────────────────────────── */

struct __packed distance_payload {
    uint16_t anchor_id;
    uint16_t tag_id;
    uint32_t distance_mm;
    uint32_t uptime_s;
    int16_t  rssi_q8;
    int16_t  fp_power_q8;
    uint16_t fp_index;
    uint16_t peak_index;
};

/* Event payload (6 bytes, little-endian):
 *   [0-1] node_id  (uint16_t)
 *   [2]   event    (uint8_t)  — UWB_EVT_* constant
 *   [3]   seq      (uint8_t)  — frame sequence number
 *   [4-5] reserved (uint16_t) — zero
 */
struct __packed event_payload {
    uint16_t node_id;
    uint8_t  event;
    uint8_t  seq;
    uint16_t reserved;
};

/* CIR payload (212 bytes, little-endian):
 *   [0-1]   anchor_id      (uint16)
 *   [2-3]   tag_id         (uint16)
 *   [4-7]   distance_mm    (uint32)
 *   [8-11]  uptime_s       (uint32)
 *   [12-13] fp_index       (uint16, Q10.6)
 *   [14-15] sample_offset  (uint16)
 *   [16]    num_samples    (uint8)
 *   [17]    read_mode      (uint8) — DWT_CIR_READ_HI=3
 *   [18-19] reserved       (uint16)
 *   [20..] cir_data        (num_samples × 4 bytes: int16 real + int16 imag)
 */
struct __packed cir_payload {
    uint16_t anchor_id;
    uint16_t tag_id;
    uint32_t distance_mm;
    uint32_t uptime_s;
    uint16_t fp_index;
    uint16_t sample_offset;
    uint8_t  num_samples;
    uint8_t  read_mode;
    uint16_t reserved;
    uint32_t cir_data[CIR_NUM_SAMPLES];
};

/* ── CoAP send work handler ──────────────────────────────────────── */

static void coap_send_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    struct distance_measurement meas;

    while (k_msgq_get(&meas_queue, &meas, K_NO_WAIT) == 0) {
        struct distance_payload payload = {
            .anchor_id   = meas.anchor_id,
            .tag_id      = meas.tag_id,
            .distance_mm = (uint32_t)(meas.distance_m * 1000.0f),
            .uptime_s    = (uint32_t)(k_uptime_get() / 1000),
            .rssi_q8     = meas.rssi_q8,
            .fp_power_q8 = meas.fp_power_q8,
            .fp_index    = meas.fp_index,
            .peak_index  = meas.peak_index,
        };

        int ret = coap_send_request(COAP_METHOD_POST,
                                    (const struct sockaddr *)&server_addr,
                                    distance_uri,
                                    (const uint8_t *)&payload,
                                    sizeof(payload),
                                    NULL);
        if (ret < 0) {
            LOG_WRN("CoAP POST /distance failed: %d", ret);
        } else {
            LOG_INF("CoAP POST /distance OK (ret=%d, %u bytes)",
                    ret, (unsigned)sizeof(payload));
        }
    }
}

/* ── CoAP event send work handler ───────────────────────────────── */

static void coap_send_evt_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    struct uwb_event_msg msg;

    while (k_msgq_get(&evt_queue, &msg, K_NO_WAIT) == 0) {
        struct event_payload payload = {
            .node_id  = msg.node_id,
            .event    = msg.event,
            .seq      = msg.seq,
            .reserved = 0,
        };

        int ret = coap_send_request(COAP_METHOD_POST,
                                    (const struct sockaddr *)&server_addr,
                                    event_uri,
                                    (const uint8_t *)&payload,
                                    sizeof(payload),
                                    NULL);
        if (ret < 0) {
            LOG_WRN("Event CoAP POST failed: %d", ret);
        }
    }
}

/* ── CoAP CIR send work handler ────────────────────────────────── */

static void coap_send_cir_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    struct cir_measurement meas;

    while (k_msgq_get(&cir_queue, &meas, K_NO_WAIT) == 0) {
        struct cir_payload payload = {
            .anchor_id     = meas.anchor_id,
            .tag_id        = meas.tag_id,
            .distance_mm   = meas.distance_mm,
            .uptime_s      = (uint32_t)(k_uptime_get() / 1000),
            .fp_index      = meas.fp_index,
            .sample_offset = meas.sample_offset,
            .num_samples   = meas.num_samples,
            .read_mode     = 3,  /* DWT_CIR_READ_HI */
            .reserved      = 0,
        };
        memcpy(payload.cir_data, meas.cir_data,
               meas.num_samples * sizeof(uint32_t));

        int ret = coap_send_request(COAP_METHOD_POST,
                                    (const struct sockaddr *)&server_addr,
                                    cir_uri,
                                    (const uint8_t *)&payload,
                                    sizeof(payload),
                                    NULL);
        if (ret < 0) {
            LOG_WRN("CIR CoAP POST failed: %d", ret);
        } else {
            LOG_DBG("CIR posted: %u samples @ offset %u",
                    meas.num_samples, meas.sample_offset);
        }
    }
}

/* ── OpenThread state change handler ────────────────────────────── */

static void thread_role_update(otDeviceRole role)
{
    switch (role) {
    case OT_DEVICE_ROLE_CHILD:
    case OT_DEVICE_ROLE_ROUTER:
    case OT_DEVICE_ROLE_LEADER:
        thread_connected = true;
        LOG_INF("Thread connected. Role: %s",
                role == OT_DEVICE_ROLE_LEADER ? "Leader" :
                role == OT_DEVICE_ROLE_ROUTER ? "Router" : "Child");
        break;
    default:
        thread_connected = false;
        LOG_INF("Thread disconnected");
        break;
    }
}

static void on_thread_state_changed(otChangedFlags flags, void *user_data)
{
    ARG_UNUSED(user_data);
    if (flags & OT_CHANGED_THREAD_ROLE) {
        struct otInstance *instance = openthread_get_default_instance();
        thread_role_update(otThreadGetDeviceRole(instance));
    }
}

static struct openthread_state_changed_callback ot_state_cb = {
    .otCallback = on_thread_state_changed,
};

/* ── Public API ───────────────────────────────────────────────────── */

int thread_coap_init(void)
{
    /* Parse server IPv6 address from runtime config */
    int ret = zsock_inet_pton(AF_INET6, g_config.server_addr,
                              &server_addr.sin6_addr);
    if (ret != 1) {
        LOG_ERR("Invalid server addr: '%s'", g_config.server_addr);
        return -EINVAL;
    }
    server_addr.sin6_port = htons(g_config.server_port);

    /* Initialize CoAP */
    coap_init(AF_INET6, NULL);

    /* Start CoAP work queue */
    k_work_queue_init(&coap_workq);
    k_work_queue_start(&coap_workq, coap_workq_stack,
                       K_THREAD_STACK_SIZEOF(coap_workq_stack),
                       COAP_WORKQ_PRIORITY, NULL);

    k_work_init(&send_work,     coap_send_work_handler);
    k_work_init(&send_evt_work, coap_send_evt_work_handler);
    k_work_init(&send_cir_work, coap_send_cir_work_handler);

    /* Register Thread state change callback and start the stack */
    openthread_state_changed_callback_register(&ot_state_cb);
    openthread_run();

    LOG_INF("Thread/CoAP initialized. Server: [%s]:%d",
            g_config.server_addr, g_config.server_port);
    return 0;
}

int thread_coap_set_server(const char *addr, uint16_t port)
{
    int ret = zsock_inet_pton(AF_INET6, addr, &server_addr.sin6_addr);
    if (ret != 1) {
        LOG_ERR("Invalid server addr: '%s'", addr);
        return -EINVAL;
    }
    server_addr.sin6_port = htons(port);
    LOG_INF("CoAP server updated: [%s]:%u", addr, port);
    return 0;
}

void thread_coap_send_event(uint16_t node_id, uint8_t event, uint8_t seq)
{
    if (!thread_connected) {
        return;
    }

    struct uwb_event_msg msg = {
        .node_id = node_id,
        .event   = event,
        .seq     = seq,
    };

    if (k_msgq_put(&evt_queue, &msg, K_NO_WAIT) != 0) {
        LOG_WRN("Event queue full, dropping");
    }

    k_work_submit_to_queue(&coap_workq, &send_evt_work);
}

void thread_coap_send_distance(uint16_t anchor_id, uint16_t tag_id,
                                float distance_m,
                                int16_t rssi_q8, int16_t fp_power_q8,
                                uint16_t fp_index, uint16_t peak_index)
{
    if (!thread_connected) {
        LOG_WRN("Distance dropped: Thread not connected");
        return;
    }

    struct distance_measurement meas = {
        .anchor_id   = anchor_id,
        .tag_id      = tag_id,
        .distance_m  = distance_m,
        .rssi_q8     = rssi_q8,
        .fp_power_q8 = fp_power_q8,
        .fp_index    = fp_index,
        .peak_index  = peak_index,
    };

    /* Drop oldest if queue is full (non-blocking put) */
    if (k_msgq_put(&meas_queue, &meas, K_NO_WAIT) != 0) {
        LOG_WRN("Measurement queue full, dropping oldest");
        struct distance_measurement dummy;
        k_msgq_get(&meas_queue, &dummy, K_NO_WAIT);
        k_msgq_put(&meas_queue, &meas, K_NO_WAIT);
    }

    LOG_INF("Queued dist: anchor=0x%04X tag=0x%04X %.3fm",
            anchor_id, tag_id, (double)distance_m);
    k_work_submit_to_queue(&coap_workq, &send_work);
}

void thread_coap_send_cir(uint16_t anchor_id, uint16_t tag_id,
                           uint32_t distance_mm,
                           uint16_t fp_index, uint16_t sample_offset,
                           const uint32_t *cir_data, uint8_t num_samples)
{
    if (!thread_connected) {
        return;
    }

    struct cir_measurement meas = {
        .anchor_id     = anchor_id,
        .tag_id        = tag_id,
        .distance_mm   = distance_mm,
        .fp_index      = fp_index,
        .sample_offset = sample_offset,
        .num_samples   = num_samples,
    };
    memcpy(meas.cir_data, cir_data, num_samples * sizeof(uint32_t));

    /* Drop if queue full — CIR is best-effort */
    if (k_msgq_put(&cir_queue, &meas, K_NO_WAIT) != 0) {
        LOG_WRN("CIR queue full, dropping");
    }

    k_work_submit_to_queue(&coap_workq, &send_cir_work);
}
