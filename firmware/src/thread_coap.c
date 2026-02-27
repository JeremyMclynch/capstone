/*
 * Thread CoAP Client - Distance Reporting
 *
 * Manages the OpenThread network connection and sends distance
 * measurement reports to the CoAP server as UDP POST requests.
 *
 * CoAP resource: POST /distance
 *
 * Binary payload (12 bytes, little-endian):
 *   [0-1]  anchor_id   (uint16_t)
 *   [2-3]  tag_id      (uint16_t)
 *   [4-7]  distance_mm (uint32_t, millimeters)
 *   [8-11] uptime_s    (uint32_t, seconds since boot)
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/coap.h>
#include <net/coap_utils.h>
#include <openthread.h>
#include <openthread/thread.h>

#include "thread_coap.h"

LOG_MODULE_REGISTER(thread_coap, LOG_LEVEL_INF);

/* CoAP resource path components */
static const char *const distance_uri[] = { "distance", NULL };
static const char *const event_uri[]    = { "event",    NULL };

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

static struct k_work send_work;
static struct k_work send_evt_work;
static bool thread_connected = false;

/* ── Binary payload encoding ─────────────────────────────────────── */

struct __packed distance_payload {
    uint16_t anchor_id;
    uint16_t tag_id;
    uint32_t distance_mm;
    uint32_t uptime_s;
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
        };

        int ret = coap_send_request(COAP_METHOD_POST,
                                    (const struct sockaddr *)&server_addr,
                                    distance_uri,
                                    (const uint8_t *)&payload,
                                    sizeof(payload),
                                    NULL);
        if (ret < 0) {
            LOG_WRN("CoAP POST failed: %d", ret);
        } else {
            LOG_DBG("Posted: anchor=0x%04X tag=0x%04X dist=%.3f m",
                    meas.anchor_id, meas.tag_id, (double)meas.distance_m);
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

/* ── OpenThread state change handler ────────────────────────────── */

static void on_thread_state_changed(otChangedFlags flags, void *user_data)
{
    ARG_UNUSED(user_data);
    struct otInstance *instance = openthread_get_default_instance();

    if (!(flags & OT_CHANGED_THREAD_ROLE)) {
        return;
    }

    otDeviceRole role = otThreadGetDeviceRole(instance);
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

static struct openthread_state_changed_callback ot_state_cb = {
    .otCallback = on_thread_state_changed,
};

/* ── Public API ───────────────────────────────────────────────────── */

int thread_coap_init(void)
{
    /* Parse server IPv6 address from Kconfig string */
    int ret = zsock_inet_pton(AF_INET6, CONFIG_COAP_SERVER_ADDR,
                              &server_addr.sin6_addr);
    if (ret != 1) {
        LOG_ERR("Invalid COAP_SERVER_ADDR: '%s'", CONFIG_COAP_SERVER_ADDR);
        return -EINVAL;
    }
    server_addr.sin6_port = htons(CONFIG_COAP_SERVER_PORT);

    /* Initialize CoAP */
    coap_init(AF_INET6, NULL);

    /* Start CoAP work queue */
    k_work_queue_init(&coap_workq);
    k_work_queue_start(&coap_workq, coap_workq_stack,
                       K_THREAD_STACK_SIZEOF(coap_workq_stack),
                       COAP_WORKQ_PRIORITY, NULL);

    k_work_init(&send_work,     coap_send_work_handler);
    k_work_init(&send_evt_work, coap_send_evt_work_handler);

    /* Register Thread state change callback and start the stack */
    openthread_state_changed_callback_register(&ot_state_cb);
    openthread_run();

    LOG_INF("Thread/CoAP initialized. Server: [%s]:%d",
            CONFIG_COAP_SERVER_ADDR, CONFIG_COAP_SERVER_PORT);
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
                                float distance_m)
{
    if (!thread_connected) {
        return;
    }

    struct distance_measurement meas = {
        .anchor_id  = anchor_id,
        .tag_id     = tag_id,
        .distance_m = distance_m,
    };

    /* Drop oldest if queue is full (non-blocking put) */
    if (k_msgq_put(&meas_queue, &meas, K_NO_WAIT) != 0) {
        LOG_WRN("Measurement queue full, dropping oldest");
        struct distance_measurement dummy;
        k_msgq_get(&meas_queue, &dummy, K_NO_WAIT);
        k_msgq_put(&meas_queue, &meas, K_NO_WAIT);
    }

    k_work_submit_to_queue(&coap_workq, &send_work);
}
