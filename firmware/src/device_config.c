/*
 * Device Configuration — NVS-backed runtime config
 *
 * On first boot (or after factory reset) all values come from Kconfig.
 * Once device_config_save() is called, values persist in NVS and override
 * Kconfig on subsequent boots.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <string.h>

#include "device_config.h"

LOG_MODULE_REGISTER(device_config, LOG_LEVEL_INF);

/* ── Global config instance ──────────────────────────────────────── */

struct device_config g_config;

/* ── Settings keys (under "uwb/" namespace) ──────────────────────── */

#define SETTINGS_KEY_ROLE      "uwb/role"
#define SETTINGS_KEY_ADDR      "uwb/addr"
#define SETTINGS_KEY_INTERVAL  "uwb/interval"
#define SETTINGS_KEY_SERVER    "uwb/server"
#define SETTINGS_KEY_PORT      "uwb/port"
#define SETTINGS_KEY_AUTOSTART "uwb/autostart"
#define SETTINGS_KEY_CAL_OFFSET "uwb/cal_offset"

/* ── Load Kconfig defaults into g_config ─────────────────────────── */

static void load_defaults(void)
{
    g_config.role = IS_ENABLED(CONFIG_NODE_ROLE_TAG) ? ROLE_TAG : ROLE_ANCHOR;
    g_config.uwb_addr = CONFIG_UWB_NODE_SHORT_ADDR;
    g_config.ranging_interval_ms = CONFIG_UWB_RANGING_INTERVAL_MS;
    strncpy(g_config.server_addr, CONFIG_COAP_SERVER_ADDR,
            sizeof(g_config.server_addr) - 1);
    g_config.server_addr[sizeof(g_config.server_addr) - 1] = '\0';
    g_config.server_port = CONFIG_COAP_SERVER_PORT;
    g_config.autostart = true;
    g_config.calibration_offset_mm = 0;
}

/* ── Settings handler (called by settings_load()) ────────────────── */

static int config_set(const char *name, size_t len, settings_read_cb read_cb,
                      void *cb_arg)
{
    if (!strcmp(name, "role")) {
        if (len != sizeof(g_config.role)) return -EINVAL;
        return read_cb(cb_arg, &g_config.role, sizeof(g_config.role));
    }
    if (!strcmp(name, "addr")) {
        if (len != sizeof(g_config.uwb_addr)) return -EINVAL;
        return read_cb(cb_arg, &g_config.uwb_addr, sizeof(g_config.uwb_addr));
    }
    if (!strcmp(name, "interval")) {
        if (len != sizeof(g_config.ranging_interval_ms)) return -EINVAL;
        return read_cb(cb_arg, &g_config.ranging_interval_ms,
                       sizeof(g_config.ranging_interval_ms));
    }
    if (!strcmp(name, "server")) {
        if (len >= sizeof(g_config.server_addr)) return -EINVAL;
        memset(g_config.server_addr, 0, sizeof(g_config.server_addr));
        return read_cb(cb_arg, g_config.server_addr, len);
    }
    if (!strcmp(name, "port")) {
        if (len != sizeof(g_config.server_port)) return -EINVAL;
        return read_cb(cb_arg, &g_config.server_port, sizeof(g_config.server_port));
    }
    if (!strcmp(name, "autostart")) {
        if (len != sizeof(g_config.autostart)) return -EINVAL;
        return read_cb(cb_arg, &g_config.autostart, sizeof(g_config.autostart));
    }
    if (!strcmp(name, "cal_offset")) {
        if (len != sizeof(g_config.calibration_offset_mm)) return -EINVAL;
        return read_cb(cb_arg, &g_config.calibration_offset_mm,
                       sizeof(g_config.calibration_offset_mm));
    }

    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(uwb, "uwb", NULL, config_set, NULL, NULL);

/* ── Public API ───────────────────────────────────────────────────── */

int device_config_init(void)
{
    /* Start with Kconfig defaults */
    load_defaults();

    /* Load NVS overrides (if any) */
    int ret = settings_subsys_init();
    if (ret) {
        LOG_ERR("settings_subsys_init failed: %d", ret);
        return ret;
    }

    ret = settings_load();
    if (ret) {
        LOG_ERR("settings_load failed: %d", ret);
        return ret;
    }

    LOG_INF("Config: role=%s addr=0x%04X interval=%u ms server=[%s]:%u auto=%d cal=%d",
            g_config.role == ROLE_TAG ? "TAG" : "ANCHOR",
            g_config.uwb_addr,
            g_config.ranging_interval_ms,
            g_config.server_addr,
            g_config.server_port,
            g_config.autostart,
            g_config.calibration_offset_mm);

    return 0;
}

int device_config_save(void)
{
    int ret = 0;
    int r;

    r = settings_save_one(SETTINGS_KEY_ROLE, &g_config.role,
                          sizeof(g_config.role));
    if (r) { LOG_ERR("save role: %d", r); ret = r; }

    r = settings_save_one(SETTINGS_KEY_ADDR, &g_config.uwb_addr,
                          sizeof(g_config.uwb_addr));
    if (r) { LOG_ERR("save addr: %d", r); ret = r; }

    r = settings_save_one(SETTINGS_KEY_INTERVAL, &g_config.ranging_interval_ms,
                          sizeof(g_config.ranging_interval_ms));
    if (r) { LOG_ERR("save interval: %d", r); ret = r; }

    r = settings_save_one(SETTINGS_KEY_SERVER, g_config.server_addr,
                          strlen(g_config.server_addr) + 1);
    if (r) { LOG_ERR("save server: %d", r); ret = r; }

    r = settings_save_one(SETTINGS_KEY_PORT, &g_config.server_port,
                          sizeof(g_config.server_port));
    if (r) { LOG_ERR("save port: %d", r); ret = r; }

    r = settings_save_one(SETTINGS_KEY_AUTOSTART, &g_config.autostart,
                          sizeof(g_config.autostart));
    if (r) { LOG_ERR("save autostart: %d", r); ret = r; }

    r = settings_save_one(SETTINGS_KEY_CAL_OFFSET, &g_config.calibration_offset_mm,
                          sizeof(g_config.calibration_offset_mm));
    if (r) { LOG_ERR("save cal_offset: %d", r); ret = r; }

    if (!ret) {
        LOG_INF("Config saved to NVS");
    }
    return ret;
}

int device_config_reset(void)
{
    int ret = 0;
    int r;

    r = settings_delete(SETTINGS_KEY_ROLE);      if (r) ret = r;
    r = settings_delete(SETTINGS_KEY_ADDR);      if (r) ret = r;
    r = settings_delete(SETTINGS_KEY_INTERVAL);  if (r) ret = r;
    r = settings_delete(SETTINGS_KEY_SERVER);    if (r) ret = r;
    r = settings_delete(SETTINGS_KEY_PORT);      if (r) ret = r;
    r = settings_delete(SETTINGS_KEY_AUTOSTART);   if (r) ret = r;
    r = settings_delete(SETTINGS_KEY_CAL_OFFSET);  if (r) ret = r;

    if (!ret) {
        LOG_INF("Config erased — defaults will be used on next boot");
    }

    /* Reload defaults into runtime struct */
    load_defaults();

    return ret;
}
