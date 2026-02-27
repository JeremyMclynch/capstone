/*
 * UWB Mesh Tracker - Main Application
 *
 * Initializes the UWB ranging subsystem and Thread/CoAP networking,
 * then connects the two via a distance measurement callback.
 *
 * Node roles are selected at compile time via Kconfig:
 *   CONFIG_NODE_ROLE_ANCHOR=y  →  DS-TWR responder, reports via CoAP
 *   CONFIG_NODE_ROLE_TAG=y     →  DS-TWR initiator
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <dk_buttons_and_leds.h>

#include "uwb_manager.h"
#include "thread_coap.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* LED assignments (matches DK and DWM3001CDK LED aliases) */
#define LED_THREAD_OK   DK_LED1   /* Solid when Thread is attached   */
#define LED_RANGING     DK_LED2   /* Blinks on each distance report   */

/* ── Distance callback (called from UWB thread) ─────────────────── */
static void on_distance_measured(uint16_t anchor_id, uint16_t tag_id,
                                  float distance_m)
{
    /* Visual indicator: blink LED2 on measurement */
    dk_set_led(LED_RANGING, 1);
    k_msleep(20);
    dk_set_led(LED_RANGING, 0);

    /* Only anchors report to the server */
    if (IS_ENABLED(CONFIG_NODE_ROLE_ANCHOR)) {
        thread_coap_send_distance(anchor_id, tag_id, distance_m);
    }
}

/* ── Application Entry Point ─────────────────────────────────────── */
int main(void)
{
    int ret;

    printk("=== UWB Mesh Tracker boot ===\n");
    LOG_INF("=== UWB Mesh Tracker ===");
    LOG_INF("Board: %s | Role: %s | Addr: 0x%04X",
            CONFIG_BOARD,
            IS_ENABLED(CONFIG_NODE_ROLE_ANCHOR) ? "ANCHOR" : "TAG",
            (unsigned int)CONFIG_UWB_NODE_SHORT_ADDR);

    /* Initialize LEDs */
    ret = dk_leds_init();
    if (ret) {
        LOG_WRN("LED init failed: %d (continuing)", ret);
    }

    /* Initialize Thread networking and CoAP client */
    ret = thread_coap_init();
    if (ret) {
        LOG_ERR("Thread/CoAP init failed: %d", ret);
        return ret;
    }

    /* Initialize DW3000 UWB transceiver */
    ret = uwb_manager_init();
    if (ret) {
        LOG_ERR("UWB manager init failed: %d", ret);
        return ret;
    }

    /* Connect distance measurements to the CoAP reporter */
    uwb_manager_set_distance_cb(on_distance_measured);

    /* Start the ranging task (returns immediately; logic runs in thread) */
    ret = uwb_manager_start();
    if (ret) {
        LOG_ERR("UWB manager start failed: %d", ret);
        return ret;
    }

    LOG_INF("Startup complete. Ranging active.");

    /* Main thread: update status LED based on Thread connection.
     * OpenThread state changes are handled via callback in thread_coap.c.
     * Here we just keep the main thread alive with a low-priority loop. */
    while (1) {
        k_msleep(5000);
    }

    return 0;
}
