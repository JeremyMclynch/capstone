#ifndef THREAD_COAP_H
#define THREAD_COAP_H

#include <stdint.h>

/* UWB event types reported by thread_coap_send_event() */
#define UWB_EVT_POLL_TX   0x01  /* Tag sent a POLL */
#define UWB_EVT_RESP_RX   0x02  /* Tag received a RESP */
#define UWB_EVT_FINAL_TX  0x03  /* Tag sent a FINAL */
#define UWB_EVT_NO_RESP   0x10  /* Tag got no RESP (timeout) */

/**
 * @brief Initialize OpenThread and the CoAP client.
 */
int thread_coap_init(void);

/**
 * @brief Enqueue a distance measurement for CoAP transmission.
 *
 * @param anchor_id   16-bit UWB short address of the anchor.
 * @param tag_id      16-bit UWB short address of the tag.
 * @param distance_m  Measured distance in meters.
 */
void thread_coap_send_distance(uint16_t anchor_id, uint16_t tag_id,
                                float distance_m);

/**
 * @brief Send a UWB event notification over CoAP (POST /event).
 *
 * Binary payload (6 bytes, little-endian):
 *   [0-1] node_id   (uint16_t) — short address of the reporting node
 *   [2]   event     (uint8_t)  — UWB_EVT_* constant
 *   [3]   seq       (uint8_t)  — frame sequence number
 *   [4-5] reserved  (uint16_t) — zero
 *
 * @param node_id  Short address of this node.
 * @param event    One of the UWB_EVT_* constants.
 * @param seq      Current frame sequence number.
 */
void thread_coap_send_event(uint16_t node_id, uint8_t event, uint8_t seq);

#endif /* THREAD_COAP_H */
