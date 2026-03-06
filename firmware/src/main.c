/*
 * UWB Mesh Tracker - Main Application
 *
 * Boot sequence:
 *   1. Load config from NVS (or Kconfig defaults on first boot)
 *   2. Initialize LEDs
 *   3. Initialize Thread/CoAP networking
 *   4. Start UCI CoAP server (remote commands over Thread)
 *   5. Initialize DW3000 UWB transceiver
 *   6. Start UCI command interface on UART
 *   7. Auto-start ranging (if config says so)
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include "leds.h"

#include "device_config.h"
#include "uwb_manager.h"
#include "thread_coap.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* UCI transport init (defined in uci_uart.c and uci_coap.c) */
extern int uci_uart_init(void);
extern int uci_coap_init(void);

/* LED assignments */
#define LED_THREAD_OK   DK_LED1
#define LED_RANGING     DK_LED2

/* ── Distance callback (called from UWB thread) ─────────────────── */
static void on_distance_measured(uint16_t anchor_id, uint16_t tag_id,
                                  float distance_m,
                                  int16_t rssi_q8, int16_t fp_power_q8,
                                  uint16_t fp_index, uint16_t peak_index)
{
    dk_set_led(LED_RANGING, 1);
    k_msleep(20);
    dk_set_led(LED_RANGING, 0);

    if (g_config.role == ROLE_ANCHOR) {
        thread_coap_send_distance(anchor_id, tag_id, distance_m,
                                  rssi_q8, fp_power_q8, fp_index, peak_index);
    }
}

/* ── Application Entry Point ─────────────────────────────────────── */
int main(void)
{
    int ret;

    printk("=== UWB Mesh Tracker boot ===\n");

    /* 1. Load persistent config */
    ret = device_config_init();
    if (ret) {
        LOG_ERR("Config init failed: %d", ret);
        return ret;
    }

    LOG_INF("Board: %s | Role: %s | Addr: 0x%04X",
            CONFIG_BOARD,
            g_config.role == ROLE_ANCHOR ? "ANCHOR" : "TAG",
            g_config.uwb_addr);

    /* 2. Initialize LEDs */
    ret = dk_leds_init();
    if (ret) {
        LOG_WRN("LED init failed: %d (continuing)", ret);
    }

    /* 3. Initialize Thread networking and CoAP client */
    ret = thread_coap_init();
    if (ret) {
        LOG_ERR("Thread/CoAP init failed: %d", ret);
        return ret;
    }

    /* 4. Start UCI CoAP server (remote commands over Thread) */
    ret = uci_coap_init();
    if (ret) {
        LOG_WRN("UCI CoAP init failed: %d (remote commands unavailable)", ret);
    }

    /* 5. Initialize DW3000 UWB transceiver */
    ret = uwb_manager_init();
    if (ret) {
        LOG_ERR("UWB manager init failed: %d", ret);
        return ret;
    }

    uwb_manager_set_distance_cb(on_distance_measured);
    uwb_manager_set_cir_cb(thread_coap_send_cir);

    /* 6. Start UCI command interface */
    ret = uci_uart_init();
    if (ret) {
        LOG_ERR("UCI UART init failed: %d", ret);
        return ret;
    }

    /* 7. Auto-start ranging if configured */
    if (g_config.autostart) {
        ret = uwb_manager_start();
        if (ret) {
            LOG_ERR("UWB manager start failed: %d", ret);
        }
    } else {
        LOG_INF("Ranging not auto-started (send UCI START command)");
    }

    /* Turn on LED1 to indicate app is running */
    dk_set_led_on(LED_THREAD_OK);

    LOG_INF("Startup complete.");

    while (1) {
        k_msleep(5000);
    }

    return 0;
}
