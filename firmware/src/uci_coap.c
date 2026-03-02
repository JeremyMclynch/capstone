/*
 * UCI CoAP Transport
 *
 * Registers a POST /cmd resource on the OpenThread CoAP server.
 * Incoming requests carry a binary UCI command (cmd + len + payload),
 * and responses carry the UCI result (cmd + status + len + payload).
 *
 * No sync byte or CRC — CoAP/UDP provides framing and integrity.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <openthread/coap.h>
#include <openthread/message.h>
#include <openthread.h>

#include "uci.h"

LOG_MODULE_REGISTER(uci_coap, LOG_LEVEL_INF);

#define COAP_PORT 5683

static otInstance *ot_instance;

/* ── Forward declaration ──────────────────────────────────────────── */

static void cmd_request_handler(void *ctx, otMessage *msg,
                                const otMessageInfo *msg_info);

static otCoapResource cmd_resource = {
    .mUriPath = "cmd",
    .mHandler = cmd_request_handler,
    .mContext = NULL,
    .mNext   = NULL,
};

/* ── POST /cmd handler ────────────────────────────────────────────── */

static void cmd_request_handler(void *ctx, otMessage *msg,
                                const otMessageInfo *msg_info)
{
    ARG_UNUSED(ctx);
    otError error;

    /* Only accept POST */
    if (otCoapMessageGetCode(msg) != OT_COAP_CODE_POST) {
        LOG_WRN("CoAP /cmd: expected POST, got code %d",
                otCoapMessageGetCode(msg));
        return;
    }

    /* Read payload: [CMD:1] [LEN:1] [PAYLOAD:0..64] */
    uint16_t offset = otMessageGetOffset(msg);
    uint16_t length = otMessageGetLength(msg) - offset;

    if (length < 2) {
        LOG_WRN("CoAP /cmd: payload too short (%u bytes)", length);
        return;
    }

    uint8_t buf[2 + UCI_MAX_PAYLOAD];
    uint16_t read_len = (length <= sizeof(buf)) ? length : sizeof(buf);
    uint16_t actual = otMessageRead(msg, offset, buf, read_len);

    if (actual < 2) {
        LOG_WRN("CoAP /cmd: failed to read payload");
        return;
    }

    /* Parse UCI request */
    struct uci_request req = {
        .cmd = buf[0],
        .len = buf[1],
    };

    if (req.len > UCI_MAX_PAYLOAD || (uint16_t)(req.len + 2) > actual) {
        LOG_WRN("CoAP /cmd: bad len=%u (payload=%u)", req.len, actual);
        return;
    }

    if (req.len > 0) {
        memcpy(req.payload, &buf[2], req.len);
    }

    LOG_INF("CoAP /cmd: cmd=0x%02X len=%u", req.cmd, req.len);

    /* Process command (thread-safe) */
    struct uci_response rsp;
    uci_process_locked(&req, &rsp);

    /* Build CoAP response */
    otMessage *response = otCoapNewMessage(ot_instance, NULL);
    if (response == NULL) {
        LOG_ERR("CoAP /cmd: failed to allocate response");
        return;
    }

    otCoapType req_type = otCoapMessageGetType(msg);
    otCoapType rsp_type = (req_type == OT_COAP_TYPE_CONFIRMABLE)
                          ? OT_COAP_TYPE_ACKNOWLEDGMENT
                          : OT_COAP_TYPE_NON_CONFIRMABLE;

    otCoapCode rsp_code = (rsp.status == UCI_STATUS_OK)
                          ? OT_COAP_CODE_CHANGED
                          : OT_COAP_CODE_BAD_REQUEST;

    error = otCoapMessageInitResponse(response, msg, rsp_type, rsp_code);
    if (error != OT_ERROR_NONE) {
        LOG_ERR("CoAP /cmd: init response failed: %d", error);
        otMessageFree(response);
        return;
    }

    /* Payload marker */
    error = otCoapMessageSetPayloadMarker(response);
    if (error != OT_ERROR_NONE) {
        otMessageFree(response);
        return;
    }

    /* Append UCI response: [CMD:1] [STATUS:1] [LEN:1] [PAYLOAD:0..N] */
    uint8_t rsp_hdr[3] = { rsp.cmd, rsp.status, rsp.len };
    error = otMessageAppend(response, rsp_hdr, sizeof(rsp_hdr));
    if (error != OT_ERROR_NONE) {
        otMessageFree(response);
        return;
    }

    if (rsp.len > 0) {
        error = otMessageAppend(response, rsp.payload, rsp.len);
        if (error != OT_ERROR_NONE) {
            otMessageFree(response);
            return;
        }
    }

    /* Send — clear mSockAddr so OT uses the source from the request */
    otMessageInfo rsp_info = *msg_info;
    memset(&rsp_info.mSockAddr, 0, sizeof(rsp_info.mSockAddr));

    error = otCoapSendResponse(ot_instance, response, &rsp_info);
    if (error != OT_ERROR_NONE) {
        LOG_WRN("CoAP /cmd: send response failed: %d", error);
        /* otCoapSendResponse frees on failure */
    }
}

/* ── Public API ───────────────────────────────────────────────────── */

int uci_coap_init(void)
{
    ot_instance = openthread_get_default_instance();
    if (!ot_instance) {
        LOG_ERR("No OpenThread instance");
        return -ENODEV;
    }

    otCoapAddResource(ot_instance, &cmd_resource);

    otError error = otCoapStart(ot_instance, COAP_PORT);
    if (error != OT_ERROR_NONE) {
        LOG_ERR("Failed to start OT CoAP server: %d", error);
        return -EIO;
    }

    LOG_INF("UCI CoAP server ready on port %d (POST /cmd)", COAP_PORT);
    return 0;
}
