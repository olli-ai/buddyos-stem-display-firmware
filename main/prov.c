#include "prov.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"

static const char *TAG = "prov";
#define NS "prov"

static prov_t s_prov;
static char s_serial[32];

prov_t *prov(void) { return &s_prov; }

static void set_str(char **dst, const char *src)
{
    free(*dst);
    *dst = (src && src[0]) ? strdup(src) : NULL;
}

void prov_set_user_jwt(const char *jwt) { set_str(&s_prov.user_jwt, jwt); }
void prov_set_mqtt_jwt(const char *jwt) { set_str(&s_prov.mqtt_jwt, jwt); }

/* A string of unknown length (the JWTs run past a kilobyte), or NULL. */
static char *get_blob_str(nvs_handle_t h, const char *key)
{
    size_t n = 0;
    if (nvs_get_str(h, key, NULL, &n) != ESP_OK || n < 2) return NULL;
    char *s = malloc(n);
    if (s && nvs_get_str(h, key, s, &n) != ESP_OK) { free(s); s = NULL; }
    return s;
}

static void get_fixed(nvs_handle_t h, const char *key, char *dst, size_t cap)
{
    size_t n = cap;
    if (nvs_get_str(h, key, dst, &n) != ESP_OK) dst[0] = '\0';
}

static bool load_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    get_fixed(h, "ssid", s_prov.ssid, sizeof(s_prov.ssid));
    get_fixed(h, "pass", s_prov.password, sizeof(s_prov.password));
    get_fixed(h, "env", s_prov.env, sizeof(s_prov.env));
    get_fixed(h, "gateway", s_prov.gateway, sizeof(s_prov.gateway));
    get_fixed(h, "mqtt_url", s_prov.mqtt_url, sizeof(s_prov.mqtt_url));
    /* A kit set up before setup carried the broker has none stored: the build's. */
    if (!s_prov.mqtt_url[0])
        snprintf(s_prov.mqtt_url, sizeof(s_prov.mqtt_url), "%s", CONFIG_BUDDY_MQTT_URI);
    s_prov.user_jwt = get_blob_str(h, "user_jwt");
    s_prov.mqtt_jwt = get_blob_str(h, "mqtt_jwt");
    uint8_t act = 0;
    nvs_get_u8(h, "activated", &act);
    s_prov.activated = act;
    nvs_close(h);
    return s_prov.ssid[0] != '\0';
}

static bool setup_forced(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "force_setup", &v); nvs_close(h); }
    return v;
}

bool prov_load(void)
{
    if (load_nvs()) {
        /* Keys and lengths only: this struct holds a password and two tokens. */
        ESP_LOGI(TAG, "provisioned: ssid '%s', env %s, gateway %s, broker %s, user token %u B, "
                 "mqtt token %u B, %s", s_prov.ssid, s_prov.env[0] ? s_prov.env : "-",
                 s_prov.gateway[0] ? s_prov.gateway : "-",
                 s_prov.mqtt_url[0] ? s_prov.mqtt_url : "-",
                 (unsigned)(s_prov.user_jwt ? strlen(s_prov.user_jwt) : 0),
                 (unsigned)(s_prov.mqtt_jwt ? strlen(s_prov.mqtt_jwt) : 0),
                 s_prov.activated ? "activated" : "not yet activated");
        return true;
    }
    if (setup_forced()) {
        ESP_LOGI(TAG, "setup requested (BOOT held): ignoring bench credentials");
        return false;
    }
    if (!CONFIG_BUDDY_WIFI_SSID[0]) return false;

    /* The bench: a laptop build with credentials in sdkconfig. Treated as already
     * activated, because the device row it names was made by hand. */
    snprintf(s_prov.ssid, sizeof(s_prov.ssid), "%s", CONFIG_BUDDY_WIFI_SSID);
    snprintf(s_prov.password, sizeof(s_prov.password), "%s", CONFIG_BUDDY_WIFI_PASS);
    snprintf(s_prov.env, sizeof(s_prov.env), "%s", CONFIG_BUDDY_MQTT_ENV);
    snprintf(s_prov.gateway, sizeof(s_prov.gateway), "%s", CONFIG_BUDDY_GATEWAY_HOST);
    snprintf(s_prov.mqtt_url, sizeof(s_prov.mqtt_url), "%s", CONFIG_BUDDY_MQTT_URI);
    prov_set_mqtt_jwt(CONFIG_BUDDY_MQTT_PASS);
#if CONFIG_BUDDY_VOICE
    prov_set_user_jwt(CONFIG_BUDDY_VOICE_TOKEN);
#endif
    s_prov.activated = true;
    s_prov.from_bench = true;
    ESP_LOGW(TAG, "not provisioned: using the bench credentials from sdkconfig");
    return true;
}

esp_err_t prov_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_str(h, "ssid", s_prov.ssid);
    nvs_set_str(h, "pass", s_prov.password);
    nvs_set_str(h, "env", s_prov.env);
    nvs_set_str(h, "gateway", s_prov.gateway);
    nvs_set_str(h, "mqtt_url", s_prov.mqtt_url);
    if (s_prov.user_jwt) nvs_set_str(h, "user_jwt", s_prov.user_jwt); else nvs_erase_key(h, "user_jwt");
    if (s_prov.mqtt_jwt) nvs_set_str(h, "mqtt_jwt", s_prov.mqtt_jwt); else nvs_erase_key(h, "mqtt_jwt");
    nvs_set_u8(h, "activated", s_prov.activated);
    nvs_erase_key(h, "force_setup");          /* a real setup has now happened */
    err = nvs_commit(h);
    nvs_close(h);
    s_prov.from_bench = false;
    return err;
}

void prov_factory_reset(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_all(h);
    nvs_set_u8(h, "force_setup", 1);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGW(TAG, "credentials erased; the next boot is a setup boot");
}

const char *prov_serial(void)
{
    if (s_prov.from_bench && CONFIG_BUDDY_MQTT_USER[0]) return CONFIG_BUDDY_MQTT_USER;
    if (!s_serial[0]) {
        /* The factory base MAC, as our other STEM firmware uses and as
         * MicroPython's machine.unique_id() returns — not the STA MAC, which only
         * happens to equal it on this chip. */
        uint8_t mac[6] = { 0 };
        esp_efuse_mac_get_default(mac);
        snprintf(s_serial, sizeof(s_serial), "ESP32-%02x%02x%02x%02x%02x%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    return s_serial;
}
