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
#define UWB_FC_DISC_POLL 0x24
#define UWB_FC_DISC_RESP 0x25

/* Broadcast destination address */
#define UWB_BROADCAST_ADDR  0xFFFF

/* Frame field indices (new format with dest_addr at 10-11, src_addr at 12-13) */
#define UWB_FRAME_FC_IDX            9   /* function code byte */
#define UWB_FRAME_DST_ADDR_IDX     10   /* destination address (2B LE) */
#define UWB_FRAME_SRC_ADDR_IDX     12   /* source address (2B LE) */
#define UWB_FINAL_POLL_TX_TS_IDX   14   /* FINAL: poll TX timestamp */
#define UWB_FINAL_RESP_RX_TS_IDX   18   /* FINAL: resp RX timestamp */
#define UWB_FINAL_FINAL_TX_TS_IDX  22   /* FINAL: final TX timestamp */

/* Legacy FINAL indices (pre-directed-ranging firmware) */
#define UWB_LEGACY_FINAL_POLL_TX_TS_IDX  10
#define UWB_LEGACY_FINAL_RESP_RX_TS_IDX  14
#define UWB_LEGACY_FINAL_FINAL_TX_TS_IDX 18

/**
 * @brief Callback type invoked when a distance measurement is complete.
 *
 * Called from the UWB thread context. Implementations should be fast
 * (e.g., submit a k_work) and must not call blocking UWB APIs.
 *
 * @param anchor_id    Anchor UWB short address.
 * @param tag_id       Tag UWB short address.
 * @param distance_m   Calibrated distance in meters.
 * @param rssi_q8      Channel RSSI in Q8.8 dBm (divide by 256 for dBm).
 * @param fp_power_q8  First-path power in Q8.8 dBm.
 * @param fp_index     First-path CIR index in Q10.6 (divide by 64).
 * @param peak_index   Peak CIR sample index.
 */
typedef void (*uwb_distance_cb_t)(uint16_t anchor_id, uint16_t tag_id,
                                   float distance_m,
                                   int16_t rssi_q8, int16_t fp_power_q8,
                                   uint16_t fp_index, uint16_t peak_index);

/**
 * @brief Callback type invoked when CIR data is captured.
 *
 * Called from the UWB thread context when CIR capture is enabled.
 *
 * @param anchor_id      Anchor UWB short address.
 * @param tag_id         Tag UWB short address.
 * @param distance_mm    Distance in mm from the same ranging cycle.
 * @param fp_index       First-path CIR index in Q10.6.
 * @param sample_offset  Starting sample index in CIR accumulator.
 * @param cir_data       128 complex samples (32-bit: int16 real + int16 imag).
 * @param num_samples    Number of samples in cir_data (128).
 */
typedef void (*uwb_cir_cb_t)(uint16_t anchor_id, uint16_t tag_id,
                               uint32_t distance_mm,
                               uint16_t fp_index, uint16_t sample_offset,
                               const uint32_t *cir_data, uint8_t num_samples);

/**
 * @brief Peer entry for multi-anchor ranging (tag only).
 */
struct uwb_peer {
    uint16_t addr;          /* 0 = empty slot */
    int16_t  rssi_q8;      /* last RSSI from discovery/ranging (Q8.8 dBm) */
    int16_t  fp_power_q8;  /* last first-path power (Q8.8 dBm) */
    uint8_t  miss_count;   /* consecutive ranging failures */
    uint8_t  flags;        /* 0x01=discovered, 0x02=manual */
};

#define UWB_PEER_FLAG_DISCOVERED  0x01
#define UWB_PEER_FLAG_MANUAL      0x02
#define UWB_PEER_MISS_THRESHOLD   3

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

/**
 * @brief Register a callback to receive CIR data captures.
 * @param cb  Callback function, or NULL to deregister.
 */
void uwb_manager_set_cir_cb(uwb_cir_cb_t cb);

/**
 * @brief Enable or disable CIR capture.
 *
 * @param enabled      true to enable, false to disable.
 * @param cycle_count  Number of cycles to capture (0 = continuous until disabled).
 */
void uwb_manager_set_cir_enabled(bool enabled, uint16_t cycle_count);

/**
 * @brief Check if CIR capture is currently enabled.
 */
bool uwb_manager_get_cir_enabled(void);

/* ── Multi-anchor peer management (tag only) ─────────────────────── */

/**
 * @brief Get the current peer list.
 * @param peers   Output array (caller provides UWB_MAX_ANCHORS entries).
 * @param count   Output: number of valid peers.
 */
void uwb_manager_get_peers(struct uwb_peer *peers, uint8_t *count);

/**
 * @brief Add a peer anchor by address.
 * @param addr  UWB short address (must not be 0 or 0xFFFF).
 * @return 0 on success, -ENOSPC if list full, -EEXIST if already present.
 */
int uwb_manager_add_peer(uint16_t addr);

/**
 * @brief Remove a peer anchor by address.
 * @param addr  UWB short address to remove.
 * @return 0 on success, -ENOENT if not found.
 */
int uwb_manager_remove_peer(uint16_t addr);

/**
 * @brief Set the discovery interval (cycles between discovery rounds).
 * @param interval  0 = disabled, N = run discovery every N ranging cycles.
 */
void uwb_manager_set_discovery_interval(uint16_t interval);

/**
 * @brief Get the current discovery interval.
 */
uint16_t uwb_manager_get_discovery_interval(void);

/**
 * @brief Trigger an immediate discovery round (next cycle).
 */
void uwb_manager_trigger_discovery(void);

#endif /* UWB_MANAGER_H */
