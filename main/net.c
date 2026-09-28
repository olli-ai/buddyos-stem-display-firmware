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
#include "face_directive.h"
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
#if CONFIG_BUDDY_TLS_PROBE
#include "esp_tls.h"
#endif

static const char *TAG = "net";
static EventGroupHandle_t s_events;
#define GOT_IP BIT0
#define MQTT_UP BIT1

/* The product broker (EMQX, behind an AWS certificate). One root rather than the
 * whole bundle, following ../buddyos-stem-c-firmware/components/stem_mqtt — the
 * in-house client that already talks to this broker. */
extern const char amazon_root_ca1_pem_start[] asm("_binary_AmazonRootCA1_pem_start");

/* Topics are the token's business: the JWT carries an EMQX ACL naming exactly these,
 * so a mismatch between this and the token's `environment` claim is denied rather
 * than misrouted. */
static char s_topic_dir[160], s_topic_evt[160];

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /* The reason, not just the fact. "Disconnected, retrying" looked identical
         * whether the access point was switched off, out of range, or had changed its
         * password — and each of those has a different fix. The codes that matter:
         * 201 no AP with that SSID in range; 15 or 204 handshake failed, which is
         * almost always a wrong password; 2 or 8 the AP dropped us. */
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        const char *why =
            d->reason == WIFI_REASON_NO_AP_FOUND            ? "no access point with that SSID in range" :
            d->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ? "handshake timed out (wrong password?)" :
            d->reason == WIFI_REASON_HANDSHAKE_TIMEOUT      ? "handshake timed out (wrong password?)" :
            d->reason == WIFI_REASON_AUTH_FAIL              ? "authentication failed" :
            d->reason == WIFI_REASON_AUTH_EXPIRE            ? "authentication expired" :
            d->reason == WIFI_REASON_ASSOC_LEAVE            ? "left the network" : "see esp_wifi_types.h";
        ESP_LOGW(TAG, "disconnected from '%.32s' (reason %d: %s, rssi %d), retrying",
                 (const char *)d->ssid, d->reason, why, d->rssi);
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
    (void)arg; (void)base;
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "mqtt connected");
        /* The moment that matters. On this product line a starved TLS handshake has
         * already been seen to misreport itself as NOT_TRUSTED, so if this line
         * never appears, read the numbers above it before believing the error. */
        mem_report("mqtt connected");
#if CONFIG_BUDDY_TLS_PROBE
        xEventGroupSetBits(s_events, MQTT_UP);
#endif
        if (s_topic_dir[0]) {
            /* Connecting proves the token was accepted; these prove the ACL in it
             * actually allows what this device needs, which is a separate question
             * and the one that bites later. */
            esp_mqtt_client_subscribe(e->client, s_topic_dir, 1);
#if CONFIG_BUDDY_DESIGN_VIA_EVENT
            /* A bench path, and an honest one. Publishing to the directive topic is
             * the gateway's privilege — a device token may only subscribe there —
             * so testing the whole flow from a laptop needs a topic the same token
             * can publish to. The event topic is that, and nothing else listens on
             * it. Off by default; a shipped device takes designs from the gateway. */
            esp_mqtt_client_subscribe(e->client, s_topic_evt, 1);
            ESP_LOGW(TAG, "bench: also taking designs from %s", s_topic_evt);
#endif
            esp_mqtt_client_publish(e->client, s_topic_evt,
                                    "{\"type\":\"hello\",\"from\":\"display-firmware\"}", 0, 1, 0);
        }
        break;
    case MQTT_EVENT_SUBSCRIBED:   ESP_LOGI(TAG, "subscribed: %s", s_topic_dir); break;
    case MQTT_EVENT_PUBLISHED:    ESP_LOGI(TAG, "published to %s", s_topic_evt); break;
    case MQTT_EVENT_DATA:
        /* A design from the website. The payload is the directive envelope; the
         * handler takes either that or a bare design, because a bench test
         * publishes one and the gateway publishes the other. */
        ESP_LOGI(TAG, "directive on %.*s (%d B)", e->topic_len, e->topic, e->total_data_len);
        if (e->current_data_offset != 0 || e->data_len != e->total_data_len) {
            /* A design split across MQTT fragments would need reassembly, and a
             * half-parsed face is worse than none. buffer.size is what prevents it. */
            ESP_LOGW(TAG, "  split across fragments (%d of %d B) — ignored; raise buffer.size",
                     e->data_len, e->total_data_len);
            break;
        }
        face_directive_receive(e->data, e->data_len);
        break;
    case MQTT_EVENT_DISCONNECTED: ESP_LOGW(TAG, "mqtt disconnected"); break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "mqtt error");
        if (e->error_handle) {
            /* Which kind of failure this is decides where to look, and the two look
             * alike from the outside. `cert verify flags` non-zero is genuinely the
             * certificate; zero with a transport error, next to a small heap, is
             * memory wearing a certificate's clothes. */
            ESP_LOGE(TAG, "  type=%d  connect return=%d  tls=0x%x  cert verify flags=0x%08x  errno=%d",
                     e->error_handle->error_type,
                     e->error_handle->connect_return_code,
                     e->error_handle->esp_tls_last_esp_err,
                     (unsigned)e->error_handle->esp_tls_cert_verify_flags,
                     e->error_handle->esp_transport_sock_errno);
            if (e->error_handle->connect_return_code == MQTT_CONNECTION_REFUSE_BAD_USERNAME)
                ESP_LOGE(TAG, "  broker refused the username or token");
            if (e->error_handle->connect_return_code == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED)
                ESP_LOGE(TAG, "  broker refused authorisation (check the token's env and acl)");
        }
        break;
    default: break;
    }
}

#if CONFIG_BUDDY_TLS_PROBE
/**
 * What a *second* TLS context costs, which is the question the voice path turns on.
 *
 * esp-webrtc brings DTLS-SRTP alongside the broker's session, and no amount of
 * arithmetic about the first one answers whether the second fits. This opens a real
 * handshake to a real host and reports the heap on both sides of it. It is a proxy —
 * DTLS is not TLS, and SRTP is extra — but it is a measured proxy, and the
 * alternative is a guess.
 */
static void tls_probe(void)
{
    mem_report("before 2nd tls");
    /* Verified against the bundled roots, so this is a *real* second handshake and
     * not a cheaper imitation of one. The bundle is flash, which this probe build can
     * afford; a shipped build carries one root and no probe. */
    esp_tls_cfg_t cfg = { .crt_bundle_attach = esp_crt_bundle_attach };
    esp_tls_t *tls = esp_tls_init();
    if (!tls) { ESP_LOGE(TAG, "2nd tls: no memory even to start"); return; }
    int rc = esp_tls_conn_http_new_sync("https://staging.api.bya.ai", &cfg, tls);
    if (rc == 1) {
        mem_report("2nd tls up");
    } else {
        ESP_LOGE(TAG, "2nd tls: handshake failed (rc=%d) — read the heap above before "
                      "believing it was the certificate", rc);
        mem_report("2nd tls failed");
    }
    esp_tls_conn_destroy(tls);
    mem_report("2nd tls closed");
}
#endif

static void net_task(void *arg)
{
    (void)arg;
    s_events = xEventGroupCreate();

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

    static char client_id[128];
    if (CONFIG_BUDDY_MQTT_USER[0]) {
        snprintf(s_topic_dir, sizeof(s_topic_dir), "%s/stem/device/%s/directive",
                 CONFIG_BUDDY_MQTT_ENV, CONFIG_BUDDY_MQTT_USER);
        snprintf(s_topic_evt, sizeof(s_topic_evt), "%s/stem/device/%s/event",
                 CONFIG_BUDDY_MQTT_ENV, CONFIG_BUDDY_MQTT_USER);
        snprintf(client_id, sizeof(client_id), "buddyos-stem-%s-%s",
                 CONFIG_BUDDY_MQTT_ENV, CONFIG_BUDDY_MQTT_USER);
    }

    esp_mqtt_client_config_t mc = {
        .broker.address.uri = CONFIG_BUDDY_MQTT_URI,
        .credentials.username = CONFIG_BUDDY_MQTT_USER[0] ? CONFIG_BUDDY_MQTT_USER : NULL,
        .credentials.authentication.password = CONFIG_BUDDY_MQTT_PASS[0] ? CONFIG_BUDDY_MQTT_PASS : NULL,
        .credentials.client_id = CONFIG_BUDDY_MQTT_USER[0] ? client_id : NULL,
        .session.keepalive = 30,
        /* The device JWT alone is ~1.1 KB, so the default 1 KB buffer cannot hold
         * the CONNECT this client sends — the failure looks like a broker that hangs
         * up rather than like a buffer that is too small. Doubled again for designs,
         * which arrive on this socket and must not be split across fragments. */
        .buffer.size = 8192,
        .network.reconnect_timeout_ms = 5000,
    };

    /* mqtts:// is the measurement worth having: the handshake is the expensive
     * moment, and on this product line a TLS handshake starved of memory has already
     * been seen to report itself as NOT_TRUSTED — sending the reader after
     * certificates rather than after RAM. */
    if (strncmp(CONFIG_BUDDY_MQTT_URI, "mqtts://", 8) == 0) {
#if CONFIG_BUDDY_MQTT_AWS_ROOT
        mc.broker.verification.certificate = amazon_root_ca1_pem_start;
        ESP_LOGI(TAG, "TLS: verifying against the embedded Amazon Root CA 1");
#else
        mc.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
        ESP_LOGI(TAG, "TLS: verifying against the bundled public roots");
#endif
    }
    if (CONFIG_BUDDY_MQTT_USER[0])
        ESP_LOGI(TAG, "connecting as %s (token %u B)", client_id,
                 (unsigned)strlen(CONFIG_BUDDY_MQTT_PASS));
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mc);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, on_mqtt, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(client));
    mem_report("mqtt started");

#if CONFIG_BUDDY_TLS_PROBE
    xEventGroupWaitBits(s_events, MQTT_UP, pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));
    vTaskDelay(pdMS_TO_TICKS(500));         /* let the broker's session settle */
    tls_probe();
#endif

    vTaskDelete(NULL);
}

void net_start(void)
{
    /* NVS first, and here rather than inside the task: the stored design is read
     * on the next line, and a task that initialises it later leaves the restore
     * reading from a partition that is not mounted yet. It failed silently, which
     * is how a Buddy forgets its face over a power cut and nobody sees why. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* The face this Buddy was last given, before the radio is even up: a design
     * that only reappears once the network does is a Buddy that boots as a stranger. */
    face_directive_restore();

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
