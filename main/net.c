/*
 * Wi-Fi, BLE setup and MQTT.
 *
 * A kit that was never set up boots into BLE setup: it advertises the same GATT
 * service as the other STEM kits, so stem-fe's SetupWithBluetooth.vue drives it
 * unchanged — scan_wifi, then {ssid, password, user_jwt, mqtt_jwt, env, gateway}.
 * Once Wi-Fi is confirmed the kit tells the app so over BLE, tears BLE down, and
 * finishes on its own over the network: it connects to the broker and announces
 * itself with device.activate, which is what creates its row in the gateway. The
 * app learns the outcome from the gateway, not from BLE (ADR 0006).
 *
 * mem_report() is called at each milestone: the faces spec gates this device type
 * on free internal SRAM with the radio up, and these lines are that number.
 */
#include "net.h"
#include "prov.h"
#include "mem_report.h"
#include "face_directive.h"
#include "voice.h"
#include "buddy_ble.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "cJSON.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"

static const char *TAG = "net";
static EventGroupHandle_t s_events;
#define GOT_IP BIT0

/* Reconnect on every drop, except while a setup attempt is deciding for itself
 * whether the password the child just typed is right. */
static volatile bool s_autoreconnect = true;
static volatile uint8_t s_last_reason;

/* The product broker (EMQX, behind an AWS certificate). One root rather than the
 * whole bundle, following the in-house MQTT client that already talks to this
 * broker on our other kits. */
extern const char amazon_root_ca1_pem_start[] asm("_binary_AmazonRootCA1_pem_start");

/* Topics are the token's business: the JWT carries an EMQX ACL naming exactly these,
 * so a mismatch between this and the token's `environment` claim is denied rather
 * than misrouted. */
static char s_topic_dir[160], s_topic_evt[160];

/* The envelope every STEM kit publishes in, field for field as
 * our other STEM firmware builds it, so the gateway's event handler needs nothing
 * new for this kit. Takes ownership of `data`. */
static char *envelope(const char *type, cJSON *data)
{
    static uint32_t seq;
    cJSON *m = cJSON_CreateObject();
    if (!m) { cJSON_Delete(data); return NULL; }
    int64_t up_ms = esp_timer_get_time() / 1000;
    char id[64];
    snprintf(id, sizeof(id), "%s-%lld-%lu", prov_serial(), (long long)up_ms, (unsigned long)++seq);
    cJSON_AddNumberToObject(m, "v", 1);
    cJSON_AddStringToObject(m, "id", id);
    cJSON_AddStringToObject(m, "type", type);
    cJSON_AddNumberToObject(m, "ts", (double)time(NULL) * 1000.0);
    cJSON_AddNumberToObject(m, "uptime_ms", (double)up_ms);
    cJSON_AddStringToObject(m, "serial", prov_serial());
    cJSON_AddItemToObject(m, "data", data ? data : cJSON_CreateObject());
    char *out = cJSON_PrintUnformatted(m);
    cJSON_Delete(m);
    return out;
}

/* Announce the kit to the gateway, which creates its row for the owner named in
 * user_jwt (create-on-activate) and answers device.activate_status. Sent on every
 * connect until that answer arrives: the gateway is idempotent about it. */
static void send_activate(esp_mqtt_client_handle_t c)
{
    cJSON *d = cJSON_CreateObject();
    if (!d) return;
    cJSON_AddStringToObject(d, "user_jwt", prov()->user_jwt ? prov()->user_jwt : "");
    cJSON_AddStringToObject(d, "device_type", "buddyos_display_kit");
    cJSON_AddStringToObject(d, "fw_version", esp_app_get_description()->version);
    char *json = envelope("device.activate", d);
    if (!json) return;
    esp_mqtt_client_publish(c, s_topic_evt, json, 0, 1, 0);
    free(json);
    ESP_LOGI(TAG, "announced device.activate as %s", prov_serial());
}

/* Anything that is not the gateway's answer to device.activate is the face's. */
static bool handle_activate_status(const char *data, int len)
{
    cJSON *j = cJSON_ParseWithLength(data, len);
    const cJSON *type = j ? cJSON_GetObjectItem(j, "type") : NULL;
    bool mine = cJSON_IsString(type) && !strcmp(type->valuestring, "device.activate_status");
    if (mine) {
        const cJSON *st = cJSON_GetObjectItem(j, "status");
        if (cJSON_IsString(st) && !strcmp(st->valuestring, "successful")) {
            ESP_LOGI(TAG, "activation confirmed by the gateway");
            prov()->activated = true;
            if (!prov()->from_bench) prov_save();
        } else {
            /* The gateway refuses a serial another account already owns: that owner
             * has to release it first. Nothing this kit can do about it but say so. */
            const cJSON *reason = cJSON_GetObjectItem(j, "reason");
            ESP_LOGE(TAG, "activation refused: %s",
                     cJSON_IsString(reason) ? reason->valuestring : "no reason given");
        }
    }
    cJSON_Delete(j);
    return mine;
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        /* Not connecting here: net_task scans first and connects after, because a
         * scan and a connect attempt fight over the radio. */
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
        s_last_reason = d->reason;
        xEventGroupClearBits(s_events, GOT_IP);
        ESP_LOGW(TAG, "disconnected from '%.32s' (reason %d: %s, rssi %d)%s",
                 (const char *)d->ssid, d->reason, why, d->rssi,
                 s_autoreconnect ? ", retrying" : "");
        if (s_autoreconnect) esp_wifi_connect();
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
            if (!prov()->activated) send_activate(e->client);
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
        if (!handle_activate_status(e->data, e->data_len))
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


/* ------------------------------------------------------------------ Wi-Fi */

typedef enum { JOIN_OK, JOIN_WRONG_PASSWORD, JOIN_NOT_FOUND, JOIN_OTHER } join_t;

/* One setup attempt: try the credentials for up to `ms`, and say which way they
 * failed. 30 s, from measurement rather than taste: a classroom access point took 33 s
 * to associate cold, and a shorter wait turned a slow network into "wrong password". */
static join_t wifi_try(const char *ssid, const char *pass, int ms)
{
    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    s_last_reason = 0;
    s_autoreconnect = false;
    int64_t until = esp_timer_get_time() + (int64_t)ms * 1000;
    while (esp_timer_get_time() < until) {
        esp_wifi_connect();
        /* Each try ends in an IP or a disconnect; retry until the budget is spent,
         * because the first attempt after a scan often fails for no reason at all. */
        if (xEventGroupWaitBits(s_events, GOT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(8000)) & GOT_IP) {
            s_autoreconnect = true;
            return JOIN_OK;
        }
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    esp_wifi_disconnect();
    switch (s_last_reason) {
    case WIFI_REASON_NO_AP_FOUND:              return JOIN_NOT_FOUND;
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_MIC_FAILURE:              return JOIN_WRONG_PASSWORD;
    default:                                   return JOIN_OTHER;
    }
}

/* Networks in range, strongest first, each SSID once. Also the diagnostic the bench
 * boot prints: "no access point with that SSID in range" has two causes with
 * different fixes — the network moved, or the board cannot hear anything at all. */
static int wifi_scan(wifi_ap_record_t *aps, int max, const char *want)
{
    wifi_scan_config_t scan = { .show_hidden = false };
    if (esp_wifi_scan_start(&scan, true) != ESP_OK) return 0;
    uint16_t n = (uint16_t)max;
    esp_wifi_scan_get_ap_records(&n, aps);           /* sorted by RSSI already */
    int kept = 0;
    bool ours = false;
    for (int i = 0; i < n; i++) {
        if (!aps[i].ssid[0]) continue;
        bool dup = false;
        for (int k = 0; k < kept && !dup; k++) dup = !strcmp((char *)aps[k].ssid, (char *)aps[i].ssid);
        if (dup) continue;
        aps[kept++] = aps[i];
        bool hit = want && !strcmp((const char *)aps[i].ssid, want);
        ours |= hit;
        ESP_LOGI(TAG, "  %s %-32.32s ch %2d  rssi %4d  auth %d", hit ? "->" : "  ",
                 (const char *)aps[i].ssid, aps[i].primary, aps[i].rssi, aps[i].authmode);
    }
    ESP_LOGI(TAG, "scan: %d networks in range", kept);
    if (!kept)
        ESP_LOGE(TAG, "scan: nothing at all in range — check the antenna "
                      "(a WROOM-1U needs one plugged into its U.FL connector)");
    else if (want && !ours)
        ESP_LOGW(TAG, "scan: '%s' is not among them — renamed, 5 GHz only, "
                      "or switched off?", want);
    return kept;
}

/* -------------------------------------------------------------- BLE setup */

static QueueHandle_t s_ble_msgs;
static char *s_setup_result;             /* the verdict, until a central takes it */

/* The NimBLE host task hands messages over and returns: anything that blocks — a
 * scan is ~4 s — must not run on it, or the notification carrying the answer can
 * never be sent. Learned the hard way on an earlier kit. */
static void on_ble_data(const char *json, void *user)
{
    (void)user;
    char *copy = strdup(json);
    if (copy && xQueueSend(s_ble_msgs, &copy, 0) != pdTRUE) {
        ESP_LOGW(TAG, "setup busy, dropped a message");
        free(copy);
    }
}

static void ble_reply(const char *type, const char *status, const char *message)
{
    cJSON *m = cJSON_CreateObject();
    if (!m) return;
    if (status)  cJSON_AddStringToObject(m, "status", status);
    if (type)    cJSON_AddStringToObject(m, "type", type);
    if (message) cJSON_AddStringToObject(m, "message", message);
    char *out = cJSON_PrintUnformatted(m);
    cJSON_Delete(m);
    if (out) { buddy_ble_write(out); free(out); }
}

static void ble_send_wifi_list(void)
{
    /* 24 is what fits a notification stream inside the app's patience. */
    static wifi_ap_record_t aps[24];
    int n = wifi_scan(aps, 24, NULL);
    cJSON *m = cJSON_CreateObject();
    cJSON *results = cJSON_CreateArray();
    if (!m || !results) { cJSON_Delete(m); cJSON_Delete(results); return; }
    cJSON_AddStringToObject(m, "status", n > 0 ? "successful" : "failed");
    cJSON_AddStringToObject(m, "type", "wifi_list");
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        if (!e) break;
        cJSON_AddStringToObject(e, "ssid", (const char *)aps[i].ssid);
        cJSON_AddNumberToObject(e, "signal", aps[i].rssi);
        cJSON_AddItemToArray(results, e);
    }
    cJSON_AddItemToObject(m, "results", results);
    char *out = cJSON_PrintUnformatted(m);
    cJSON_Delete(m);
    if (out) { buddy_ble_write(out); free(out); }
}

/* The app's whole verdict rides on one unacknowledged notification, sent just after
 * the Wi-Fi join fought BLE for the radio. An earlier kit lost it often enough that
 * the app told users "check your password" about working kits, so: hold it until it
 * has gone out on a live link, advertising again if the link dropped, for up to
 * `wait_s`. Returns at once on the happy path. */
static void deliver_setup_result(int wait_s)
{
    for (int i = 0; i < wait_s * 10 && s_setup_result; i++) {
        if (buddy_ble_is_connected() && buddy_ble_write(s_setup_result) == ESP_OK) {
            free(s_setup_result);
            s_setup_result = NULL;
            ESP_LOGI(TAG, "setup result delivered");
            break;
        }
        if (!buddy_ble_is_connected() && !buddy_ble_is_advertising()) buddy_ble_enable();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (s_setup_result) {
        ESP_LOGW(TAG, "no central took the setup result in %d s; carrying on", wait_s);
        free(s_setup_result);
        s_setup_result = NULL;
    }
}

static const char *str_of(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* One message from the app. True once the kit is on Wi-Fi with what it was told. */
static bool handle_setup_message(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    const cJSON *params = root ? cJSON_GetObjectItem(root, "params") : NULL;
    if (!cJSON_IsObject(params)) {
        cJSON_Delete(root);
        ble_reply(NULL, "failed", "Invalid message");
        return false;
    }
    /* Keys only: this object carries the Wi-Fi password and two tokens. */
    char keys[160] = { 0 };
    for (const cJSON *k = params->child; k; k = k->next)
        if (k->string) {
            strncat(keys, k->string, sizeof(keys) - strlen(keys) - 2);
            strncat(keys, " ", sizeof(keys) - strlen(keys) - 1);
        }
    ESP_LOGI(TAG, "setup message: %s", keys);

    bool done = false;
    const char *ssid = str_of(params, "ssid");
    if (cJSON_GetObjectItem(params, "disconnect")) {
        /* The app is closing its dialog. Drop the link but stay discoverable: a
         * kit that is not set up has nothing else to be doing. */
        buddy_ble_disable();
        vTaskDelay(pdMS_TO_TICKS(500));
        buddy_ble_enable();
    } else if (cJSON_GetObjectItem(params, "scan_wifi")) {
        ble_send_wifi_list();
    } else if (ssid && ssid[0]) {
        prov_t *p = prov();
        const char *pass = str_of(params, "password");
        const char *env = str_of(params, "env");
        const char *gw = str_of(params, "gateway");
        /* `user_jwt` is the current name; older app builds send `access_token`. */
        const char *ujwt = str_of(params, "user_jwt");
        if (!ujwt) ujwt = str_of(params, "access_token");
        snprintf(p->ssid, sizeof(p->ssid), "%s", ssid);
        snprintf(p->password, sizeof(p->password), "%s", pass ? pass : "");
        snprintf(p->env, sizeof(p->env), "%s", env && env[0] ? env : CONFIG_BUDDY_MQTT_ENV);
        /* An app older than this kit sends no gateway; the bench host is the best
         * guess, and a wrong one only costs the voice path, never setup itself. */
        snprintf(p->gateway, sizeof(p->gateway), "%s", gw && gw[0] ? gw : CONFIG_BUDDY_GATEWAY_HOST);
        prov_set_user_jwt(ujwt);
        prov_set_mqtt_jwt(str_of(params, "mqtt_jwt"));
        p->activated = false;
        p->from_bench = false;
        if (!gw || !gw[0]) ESP_LOGW(TAG, "setup sent no gateway; using %s", p->gateway);

        ESP_LOGI(TAG, "joining '%s' (env %s, gateway %s)", p->ssid, p->env, p->gateway);
        join_t r = wifi_try(p->ssid, p->password, 30000);
        if (r == JOIN_OK) {
            /* Saved only now: a mistyped password is not worth keeping. */
            prov_save();
            s_setup_result = strdup("{\"status\":\"successful\",\"type\":\"wifi_connection\","
                                    "\"message\":\"Wi-Fi connected; finishing setup over the network\"}");
            /* Five seconds, not more: the app's real answer comes from the gateway,
             * and every second here is a second before the kit can produce it. */
            deliver_setup_result(5);
            /* The chunks were accepted for transmission, not yet sent. */
            vTaskDelay(pdMS_TO_TICKS(500));
            done = true;
        } else {
            /* The app shows a different screen per cause, so give it the cause. */
            const char *code = r == JOIN_WRONG_PASSWORD ? "10002"
                             : r == JOIN_NOT_FOUND      ? "10004" : "10006";
            ESP_LOGW(TAG, "setup join failed (code %s, last reason %d)", code, s_last_reason);
            ble_reply("wifi_connection", "failed", code);
        }
    } else {
        ble_reply(NULL, "failed", "Invalid message");
    }
    cJSON_Delete(root);
    return done;
}

/* Until the app has handed over a network that works. */
static void run_setup(void)
{
    s_ble_msgs = xQueueCreate(2, sizeof(char *));
    buddy_ble_init(prov_serial(), on_ble_data, NULL, NULL);
    if (buddy_ble_enable() != ESP_OK) {
        ESP_LOGE(TAG, "BLE would not start; this kit cannot be set up (reboot to retry)");
        vTaskDelay(portMAX_DELAY);
    }
    mem_report("ble setup");
    ESP_LOGI(TAG, "waiting to be set up: advertising as %s", prov_serial());
    for (;;) {
        char *msg = NULL;
        xQueueReceive(s_ble_msgs, &msg, portMAX_DELAY);
        bool done = handle_setup_message(msg);
        free(msg);
        if (done) break;
    }
    /* BLE's work is done, and its controller holds internal RAM the broker's and the
     * voice socket's TLS handshakes want. A kit that needs setup again gets it by
     * holding BOOT, which reboots. */
    buddy_ble_shutdown();
    mem_report("ble down");
}

#if CONFIG_BUDDY_BOOT_RESET
/* BOOT held for 5 s: forget everything and come back up in setup. The factory-reset
 * gesture our other kits use, on the one button every S3 board has. */
static void boot_button_task(void *arg)
{
    (void)arg;
    gpio_config_t in = { .pin_bit_mask = 1ULL << GPIO_NUM_0, .mode = GPIO_MODE_INPUT,
                         .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&in);
    int held = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        held = gpio_get_level(GPIO_NUM_0) == 0 ? held + 1 : 0;
        if (held == 10) ESP_LOGW(TAG, "BOOT held: keep holding for setup (5 s)");
        if (held == 50) {
            prov_factory_reset();
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }
    }
}
#endif

/* ------------------------------------------------------------------- main */

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
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    if (!prov_load()) {
        run_setup();                     /* returns on Wi-Fi, with prov() filled in */
    } else {
        prov_t *p = prov();
        static wifi_ap_record_t aps[20];
        wifi_scan(aps, 20, p->ssid);
        wifi_config_t wc = { 0 };
        strncpy((char *)wc.sta.ssid, p->ssid, sizeof(wc.sta.ssid) - 1);
        strncpy((char *)wc.sta.password, p->password, sizeof(wc.sta.password) - 1);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
        /* Stored credentials that will not connect: keep trying. A kit switched on
         * before its router must come up on its own; re-setup is BOOT's job. */
        s_autoreconnect = true;
        esp_wifi_connect();
        mem_report("wifi started");
        xEventGroupWaitBits(s_events, GOT_IP, pdFALSE, pdTRUE, portMAX_DELAY);
    }

    prov_t *p = prov();
    const char *serial = prov_serial();
    static char client_id[128];
    snprintf(s_topic_dir, sizeof(s_topic_dir), "%s/stem/device/%s/directive", p->env, serial);
    snprintf(s_topic_evt, sizeof(s_topic_evt), "%s/stem/device/%s/event", p->env, serial);
    /* The prefix EMQX's presence rules match on, as the other kits use it. */
    snprintf(client_id, sizeof(client_id), "buddyos-stem-%s-%s", p->env, serial);

    esp_mqtt_client_config_t mc = {
        .broker.address.uri = CONFIG_BUDDY_MQTT_URI,
        .credentials.username = serial,
        .credentials.authentication.password = p->mqtt_jwt,
        .credentials.client_id = client_id,
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
    if (!p->mqtt_jwt) ESP_LOGE(TAG, "no MQTT token: the broker will refuse this kit");
    ESP_LOGI(TAG, "connecting as %s (token %u B)", client_id,
             (unsigned)(p->mqtt_jwt ? strlen(p->mqtt_jwt) : 0));
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mc);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, on_mqtt, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(client));
    mem_report("mqtt started");

#if CONFIG_BUDDY_VOICE
    /* Voice needs the network and nothing from the broker, so it starts here rather
     * than waiting for MQTT to connect. The kit's serial names it to the gateway. */
    voice_start(serial, p->gateway, p->user_jwt);
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

    /* Always started when CONFIG_BUDDY_NET is on: with no credentials it runs setup.
     *
     * An earlier version returned here when CONFIG_BUDDY_WIFI_SSID was empty — and
     * because that macro is a string *literal*, the test folded to a compile-time
     * constant, the compiler proved the task unreachable, and the linker quietly
     * dropped the whole Wi-Fi and MQTT stack. The build still said "complete" and
     * the size figures looked wonderful, which is the worst way for a measurement
     * rig to be wrong. */
    /* 6 KB: setup parses the ~2.4 KB credentials message with cJSON on this stack. */
    xTaskCreate(net_task, "net", 6144, NULL, 5, NULL);
#if CONFIG_BUDDY_BOOT_RESET
    xTaskCreate(boot_button_task, "boot_btn", 2560, NULL, 2, NULL);
#endif
}
