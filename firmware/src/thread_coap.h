#ifndef THREAD_COAP_H
#define THREAD_COAP_H

#include <stdint.h>

/**
 * @brief Initialize OpenThread and the CoAP client.
 *
 * Starts the OpenThread stack and configures it with the network
 * parameters from Kconfig (network key, channel, PAN ID).
 * Also initializes the CoAP client for sending distance reports.
 *
 * @return 0 on success, negative errno on failure.
 */
int thread_coap_init(void);

/**
 * @brief Enqueue a distance measurement for CoAP transmission.
 *
 * Thread-safe. If the Thread network is not yet connected, the
 * measurement is silently dropped. The actual CoAP POST is
 * submitted to a work queue and sent asynchronously.
 *
 * @param anchor_id   16-bit UWB short address of the anchor.
 * @param tag_id      16-bit UWB short address of the tag.
 * @param distance_m  Measured distance in meters.
 */
void thread_coap_send_distance(uint16_t anchor_id, uint16_t tag_id,
                                float distance_m);

#endif /* THREAD_COAP_H */
