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
 */
typedef void (*uwb_distance_cb_t)(uint16_t anchor_id, uint16_t tag_id,
                                   float distance_m);

/**
 * @brief Status snapshot returned by uwb_manager_get_status().
 */
struct uwb_status {
    bool     running;           /* true if ranging is active */
    uint32_t last_distance_mm;  /* last measured distance in mm (0 if none) */
    uint32_t range_count;       /* total successful ranging cycles */
};

/**
 * @brief Initialize the DW3000 UWB transceiver and prepare for ranging.
 * @return 0 on success, negative errno on failure.
 */
int uwb_manager_init(void);

/**
 * @brief Register a callback to receive distance measurement results.
 * @param cb  Callback function, or NULL to deregister.
 */
void uwb_manager_set_distance_cb(uwb_distance_cb_t cb);

/**
 * @brief Start the UWB ranging task.
 * @return 0 on success, negative errno on failure.
 */
int uwb_manager_start(void);

/**
 * @brief Stop the UWB ranging task.
 * The thread remains alive but enters an idle sleep loop.
 */
void uwb_manager_stop(void);

/**
 * @brief Set the ranging interval at runtime (tag only).
 * @param interval_ms  Interval in milliseconds (50–10000).
 */
void uwb_manager_set_interval(uint16_t interval_ms);

/**
 * @brief Get current UWB status snapshot.
 * @param status  Output structure filled with current state.
 */
void uwb_manager_get_status(struct uwb_status *status);

/**
 * @brief Get last measured distance in mm (after calibration offset).
 * @return Distance in mm, or 0 if no measurement yet.
 */
uint32_t uwb_manager_get_last_distance_mm(void);

#endif /* UWB_MANAGER_H */
