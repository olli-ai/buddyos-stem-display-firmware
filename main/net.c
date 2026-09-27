/*
 * Wi-Fi and MQTT, for one reason: to find out what they cost.
 *
 * The faces spec gates this device type on free internal SRAM *with the radio up*,
 * and until now that has been an argument rather than a number. Nothing here talks
 * to the face — no design is fetched yet — because the measurement is worth taking
 * before the feature is built on top of it.
 */
#include "net.h"
#include "mem_report.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"

static const char *TAG = "net";
static EventGroupHandle_t s_events;
#define GOT_IP BIT0

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "disconnected, retrying");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        mem_report("wifi connected");
        xEventGroupSetBits(s_events, GOT_IP);
    }
}

static void on_mqtt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "mqtt connected");
        /* The moment that matters. On this product line a starved TLS handshake has
         * already been seen to misreport itself as NOT_TRUSTED, so if this line
         * never appears, read the numbers above it before believing the error. */
        mem_report("mqtt connected");
        break;
    case MQTT_EVENT_DISCONNECTED: ESP_LOGW(TAG, "mqtt disconnected"); break;
    case MQTT_EVENT_ERROR:        ESP_LOGE(TAG, "mqtt error"); break;
    default: break;
    }
}

static void net_task(void *arg)
{
    (void)arg;
    s_events = xEventGroupCreate();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    mem_report("wifi init");

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL, NULL));

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, CONFIG_BUDDY_WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, CONFIG_BUDDY_WIFI_PASS, sizeof(wc.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    mem_report("wifi started");

    if (wc.sta.ssid[0] == '\0') {
        /* No credentials, but the stack is up and that is a real measurement: it is
         * what Wi-Fi costs before a single packet. Left running rather than torn
         * down, so `running` reports include it. */
        ESP_LOGW(TAG, "no SSID set — Wi-Fi is up but will not associate.");
        ESP_LOGW(TAG, "set one with: idf.py menuconfig  (BuddyOS display firmware)");
        vTaskDelete(NULL);
        return;
    }

    xEventGroupWaitBits(s_events, GOT_IP, pdFALSE, pdTRUE, portMAX_DELAY);

    esp_mqtt_client_config_t mc = { .broker.address.uri = CONFIG_BUDDY_MQTT_URI };
    /* mqtts:// verifies against IDF's bundled public roots. That is the measurement
     * worth having: the handshake is the expensive moment, and on this product line a
     * TLS handshake starved of memory has already been seen to report itself as
     * NOT_TRUSTED — sending the reader after certificates rather than after RAM. */
    if (strncmp(CONFIG_BUDDY_MQTT_URI, "mqtts://", 8) == 0) {
        mc.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
        ESP_LOGI(TAG, "TLS: verifying against the bundled public roots");
    }
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mc);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, on_mqtt, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(client));
    mem_report("mqtt started");

    vTaskDelete(NULL);
}

void net_start(void)
{
    /* Always started when CONFIG_BUDDY_NET is on, even with no credentials.
     *
     * An earlier version returned here when CONFIG_BUDDY_WIFI_SSID was empty — and
     * because that macro is a string *literal*, the test folded to a compile-time
     * constant, the compiler proved the task unreachable, and the linker quietly
     * dropped the whole Wi-Fi and MQTT stack. The build still said "complete" and
     * the size figures looked wonderful, which is the worst way for a measurement
     * rig to be wrong. */
    xTaskCreate(net_task, "net", 5120, NULL, 5, NULL);
}
