#ifndef DEVICE_CONFIG_H
#define DEVICE_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

/* Node roles */
#define ROLE_ANCHOR 0
#define ROLE_TAG    1

/* Maximum IPv6 address string length (e.g. "fdde:ad00:beef::1") */
#define SERVER_ADDR_MAX_LEN 46

struct device_config {
    uint8_t  role;                          /* ROLE_ANCHOR or ROLE_TAG */
    uint16_t uwb_addr;                     /* UWB short address */
    uint16_t ranging_interval_ms;          /* Interval between ranging cycles */
    char     server_addr[SERVER_ADDR_MAX_LEN]; /* CoAP server IPv6 string */
    uint16_t server_port;                  /* CoAP server UDP port */
    bool     autostart;                    /* Auto-start ranging on boot */
    int16_t  calibration_offset_mm;        /* Distance calibration offset (mm) */
    uint16_t discovery_interval;           /* Discovery every N cycles (0=disabled) */
};

/* Global runtime config — initialized by device_config_init() */
extern struct device_config g_config;

/**
 * @brief Load config from NVS. Missing keys fall back to Kconfig defaults.
 * Must be called once early in main(), before other subsystems read g_config.
 * @return 0 on success, negative errno on failure.
 */
int device_config_init(void);

/**
 * @brief Persist current g_config to NVS flash.
 * @return 0 on success, negative errno on failure.
 */
int device_config_save(void);

/**
 * @brief Erase all saved config from NVS, restoring Kconfig defaults.
 * Does NOT reboot — caller should reboot after this returns.
 * @return 0 on success, negative errno on failure.
 */
int device_config_reset(void);

#endif /* DEVICE_CONFIG_H */
