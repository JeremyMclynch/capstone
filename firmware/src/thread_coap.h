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
 * @param anchor_id    16-bit UWB short address of the anchor.
 * @param tag_id       16-bit UWB short address of the tag.
 * @param distance_m   Measured distance in meters.
 * @param rssi_q8      Channel RSSI in Q8.8 dBm.
 * @param fp_power_q8  First-path power in Q8.8 dBm.
 * @param fp_index     First-path CIR index (Q10.6).
 * @param peak_index   Peak CIR sample index.
 */
void thread_coap_send_distance(uint16_t anchor_id, uint16_t tag_id,
                                float distance_m,
                                int16_t rssi_q8, int16_t fp_power_q8,
                                uint16_t fp_index, uint16_t peak_index);

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

/**
 * @brief Update the CoAP server address at runtime.
 *
 * @param addr  IPv6 address string (e.g. "ff03::1").
 * @param port  UDP port number.
 * @return 0 on success, -EINVAL if address is invalid.
 */
int thread_coap_set_server(const char *addr, uint16_t port);

#endif /* THREAD_COAP_H */
