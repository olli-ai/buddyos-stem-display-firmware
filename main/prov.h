#pragma once
#include <stdbool.h>
#include "esp_err.h"

/**
 * What a kit is told during setup, and keeps.
 *
 * Written by the BLE setup flow (net.c), which carries exactly what stem-fe's
 * SetupWithBluetooth.vue streams: ssid, password, user_jwt, mqtt_jwt, env, and —
 * new with this kit — gateway, the stem-gateway host the voice socket dials.
 *
 * Three sources, in this order:
 *   1. NVS, from a real setup. Always wins.
 *   2. The bench credentials in sdkconfig (CONFIG_BUDDY_WIFI_SSID and friends),
 *      only when nothing was provisioned, so a laptop build keeps working.
 *   3. Neither: the kit boots into setup and advertises over BLE.
 * Holding BOOT for 5 s erases (1) and ignores (2) until the next setup finishes.
 */
typedef struct {
    char ssid[33];
    char password[65];
    char env[16];            /* topic namespace; must match the MQTT token's ACL */
    char gateway[96];        /* host only, e.g. staging.stem-gateway.aws.iviet.com */
    char *user_jwt;          /* the owner's JWT: activation, and the voice route */
    char *mqtt_jwt;          /* the device's broker password (EMQX JWT with ACL) */
    bool activated;          /* the gateway has answered device.activate */
    bool from_bench;         /* came from sdkconfig, not from a setup */
} prov_t;

prov_t *prov(void);

/** Load from NVS, or the bench fallback. True if the kit has somewhere to connect. */
bool prov_load(void);
esp_err_t prov_save(void);
void prov_set_user_jwt(const char *jwt);
void prov_set_mqtt_jwt(const char *jwt);

/** Forget the network and tokens, and boot into setup until one completes. */
void prov_factory_reset(void);

/** "ESP32-" + the factory MAC, lowercase — the same serial the other kits use, so
 *  the gateway, the app's BLE filter and the MQTT ACL all agree on it. A bench build
 *  running on sdkconfig credentials uses CONFIG_BUDDY_MQTT_USER instead, since that
 *  is the serial its token was minted for. */
const char *prov_serial(void);
