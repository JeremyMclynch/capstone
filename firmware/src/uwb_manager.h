#ifndef UWB_MANAGER_H
#define UWB_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

/* Maximum anchors a tag will range against */
#define UWB_MAX_ANCHORS 8

/* UWB frame function codes */
#define UWB_FC_POLL     0x21
#define UWB_FC_RESP     0x10
#define UWB_FC_FINAL    0x23

/* Broadcast destination address */
#define UWB_BROADCAST_ADDR  0xFFFF

/**
 * @brief Callback type invoked when a distance measurement is complete.
 *
 * Called from the UWB thread context. Implementations should be fast
 * (e.g., submit a k_work) and must not call blocking UWB APIs.
 *
 * @param anchor_id  16-bit UWB short address of the anchor.
 * @param tag_id     16-bit UWB short address of the tag.
 * @param distance_m Measured distance in meters.
 */
typedef void (*uwb_distance_cb_t)(uint16_t anchor_id, uint16_t tag_id,
                                   float distance_m);

/**
 * @brief Initialize the DW3000 UWB transceiver and prepare for ranging.
 *
 * Must be called once during application init before starting the
 * UWB task. Configures SPI, resets the chip, and programs the RF.
 *
 * @return 0 on success, negative errno on failure.
 */
int uwb_manager_init(void);

/**
 * @brief Register a callback to receive distance measurement results.
 *
 * Only one callback can be registered at a time.
 *
 * @param cb  Callback function, or NULL to deregister.
 */
void uwb_manager_set_distance_cb(uwb_distance_cb_t cb);

/**
 * @brief Start the UWB ranging task.
 *
 * Launches a dedicated Zephyr thread that runs the ranging state
 * machine. Call after uwb_manager_init().
 *
 * @return 0 on success, negative errno on failure.
 */
int uwb_manager_start(void);

#endif /* UWB_MANAGER_H */
