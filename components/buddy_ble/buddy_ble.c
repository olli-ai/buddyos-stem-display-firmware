/*
 * Copied from ../buddyos-stem-c-firmware/components/stem_ble (13e9490) and renamed,
 * so the display kit speaks exactly the GATT contract stem-fe's setup flow already
 * drives. Changes: the serial is passed in (no stem_board), ESP_LOGI for STEM_LOGI.
 * Every comment below is the original's and records something measured on that
 * kit's hardware; keep them when changing this.
 */
#include "buddy_ble.h"
#include "esp_log.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "os/os_mbuf.h"
#include "esp_heap_caps.h"

static const char *TAG = "ble";

#define MANUFACTURER_ID   0x4F4C
/*
 * Notification payload sizing.
 *
 * 16 bytes was the floor this service shipped with, chosen to match what the
 * app's reassembly was written against before the negotiated MTU was tracked.
 * It is safe but very expensive: a Wi-Fi scan list is ~650 bytes, which at 16
 * bytes and a 10 ms gap is 41 notifications spread over nearly a second. That
 * is a long time to keep a link alive while the Wi-Fi radio is competing for
 * the same band, and a link that dies partway leaves the app with half a JSON
 * document and no way to tell that is what happened.
 *
 * The MTU is known -- the central negotiates it right after connecting and
 * BLE_GAP_EVENT_MTU records it -- so use it: ATT_MTU minus the 3-byte
 * notification header. At the 256 the browser negotiates that turns those 41
 * notifications into 3. The floor still applies before the exchange, where the
 * spec guarantees only 23.
 */
#define CHUNK_MIN         16
#define CHUNK_MAX         244
/* Drop a half-received message after this long with no further chunk. A
 * partial message holds the Wi-Fi password and both JWTs, so it is not
 * something to keep lying around waiting for a central that went away. */
#define CHUNK_TIMEOUT_MS  5000
#define RX_BUF_MAX        4096

/* 1a9e1774-8b71-11ef-a8cc-b25737277ce0, little-endian as NimBLE wants it. */
static const ble_uuid128_t SVC_UUID =
    BLE_UUID128_INIT(0xe0, 0x7c, 0x27, 0x37, 0x57, 0xb2, 0xcc, 0xa8,
                     0xef, 0x11, 0x71, 0x8b, 0x74, 0x17, 0x9e, 0x1a);
/* 276fb4e4-8b71-11ef-a8cc-b25737277ce0 */
static const ble_uuid128_t CHR_UUID =
    BLE_UUID128_INIT(0xe0, 0x7c, 0x27, 0x37, 0x57, 0xb2, 0xcc, 0xa8,
                     0xef, 0x11, 0x71, 0x8b, 0xe4, 0xb4, 0x6f, 0x27);

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_val_handle;
static uint16_t s_mtu;          /* 0 until this central has negotiated one */
static bool s_advertising;
static bool s_want_adv;
static bool s_host_synced;

static char s_serial[32];       /* the advertised name; its last 5 chars are the filter */
static buddy_ble_data_cb_t   s_on_data;
static buddy_ble_connect_cb_t s_on_connect;
static void *s_user;

static char  *s_rx;
static size_t s_rx_len;
static TickType_t s_rx_last;

static void start_advertising(void);

/* ------------------------------------------------------------------ inbound */

static void rx_reset(void)
{
    s_rx_len = 0;
}

static void rx_append(const uint8_t *data, size_t len)
{
    if (!s_rx) {
        s_rx = malloc(RX_BUF_MAX);
        if (!s_rx) return;
    }
    TickType_t now = xTaskGetTickCount();
    if (s_rx_len && (now - s_rx_last) > pdMS_TO_TICKS(CHUNK_TIMEOUT_MS)) {
        /* Length only, never the bytes. */
        ESP_LOGI(TAG, "stale partial message, discarded %u bytes", (unsigned)s_rx_len);
        rx_reset();
    }
    s_rx_last = now;

    if (s_rx_len + len >= RX_BUF_MAX) {
        ESP_LOGI(TAG, "message over %d bytes, dropped", RX_BUF_MAX);
        rx_reset();
        return;
    }
    memcpy(s_rx + s_rx_len, data, len);
    s_rx_len += len;
    s_rx[s_rx_len] = '\0';

    /* Messages are JSON terminated by '}'. Waiting for that byte rather than
     * parsing every chunk keeps a 2.4 KB credentials message from being parsed
     * 150 times on its way in. */
    if (len && data[len - 1] == '}') {
        char *msg = s_rx;
        size_t n = s_rx_len;
        rx_reset();
        if (s_on_data) {
            /* Hand over a copy: the callback runs long (Wi-Fi scan, connect)
             * and more chunks can arrive underneath it. */
            char *copy = malloc(n + 1);
            if (copy) {
                memcpy(copy, msg, n + 1);
                s_on_data(copy, s_user);
                free(copy);
            }
        }
    }
}

static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t buf[512];
        uint16_t len = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) == 0)
            rx_append(buf, len);
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR)
        return 0;   /* readable so the app can probe it; carries nothing */
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_chr_def CHRS[] = {
    {
        .uuid       = &CHR_UUID.u,
        .access_cb  = chr_access,
        .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                      BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_val_handle,
    },
    { 0 },
};

static const struct ble_gatt_svc_def SVCS[] = {
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &SVC_UUID.u,
        .characteristics = CHRS,
    },
    { 0 },
};

/* ----------------------------------------------------------------- outbound */

/*
 * What the stack has left, in one line, at the three moments it matters.
 *
 * os_msys is NimBLE's mbuf pool: every notification sent and every write
 * received is allocated from it, and when it runs dry ble_gatts_notify_custom()
 * returns BLE_HS_ENOMEM -- which is what truncates a transfer partway through.
 * Internal heap is the other half, since the controller, the camera and TLS all
 * compete for it.
 *
 * Printed on connect and disconnect so a leak reads as a ratchet across
 * successive connections rather than a number needing interpretation. Boards
 * that had been through a run of connect/disconnect churn started refusing
 * service discovery until they were rebooted; if that is resource exhaustion
 * these numbers will fall connection by connection, and if they are flat it is
 * not, and the search moves elsewhere.
 */
static void log_ble_resources(const char *when)
{
    ESP_LOGI(TAG, "%s: mbufs %d/%d free, internal heap %u B (largest %u)",
              when, os_msys_num_free(), os_msys_count(),
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
              (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

static size_t chunk_len(void)
{
    if (s_mtu < CHUNK_MIN + 3) return CHUNK_MIN;   /* not negotiated yet */
    size_t n = (size_t)s_mtu - 3;
    return n > CHUNK_MAX ? CHUNK_MAX : n;
}

esp_err_t buddy_ble_write(const char *json)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !json) return ESP_ERR_INVALID_STATE;
    size_t total = strlen(json);
    size_t step  = chunk_len();
    ESP_LOGI(TAG, "notifying central: %u bytes in %u-byte chunks",
              (unsigned)total, (unsigned)step);

    for (size_t off = 0; off < total; off += step) {
        size_t n = total - off;
        if (n > step) n = step;

        /* ENOMEM here means the controller's buffers are full, not that the
         * link is gone: back off and retry rather than losing the tail of a
         * scan list. Ten tries is ~100 ms, well inside the app's patience. */
        int rc = BLE_HS_ENOMEM;
        for (int attempt = 0; attempt < 10 && rc == BLE_HS_ENOMEM; attempt++) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(json + off, n);
            if (!om) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
            rc = ble_gatts_notify_custom(s_conn, s_val_handle, om);
            if (rc == BLE_HS_ENOMEM) vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (rc != 0) {
            ESP_LOGI(TAG, "notify failed at byte %u (rc=%d)", (unsigned)off, rc);
            log_ble_resources("at notify failure");
            return ESP_FAIL;
        }
        /* Pace to match the MicroPython side. Without a gap the controller
         * silently coalesces or drops, and the app sees truncated JSON. */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

/* ---------------------------------------------------------------------- gap */

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            s_advertising = false;
            s_mtu = 0;          /* each central negotiates its own */
            ESP_LOGI(TAG, "central connected");
            log_ble_resources("on connect");
            /*
             * The parameters the central chose, because they decide how much RF
             * trouble this link survives. Supervision timeout is the one that
             * matters: it is how long the two sides may hear nothing from each
             * other before giving up, and a disconnect with reason 520
             * (BLE_HS_HCI_ERR(0x08), connection timeout) means it elapsed.
             * Units are the spec's: interval x1.25 ms, timeout x10 ms.
             */
            {
                struct ble_gap_conn_desc d;
                if (ble_gap_conn_find(s_conn, &d) == 0)
                    ESP_LOGI(TAG, "conn params: itvl %u (%u ms), latency %u, "
                                   "timeout %u (%u ms)",
                              d.conn_itvl, (unsigned)(d.conn_itvl * 125 / 100),
                              d.conn_latency,
                              d.supervision_timeout,
                              (unsigned)(d.supervision_timeout * 10));
            }
            /*
             * Ask for a supervision timeout this link can actually survive.
             *
             * macOS and iOS hand out 720 ms, measured. That is the entire
             * budget for RF trouble before the link is declared dead, and this
             * device spends its BLE life doing the two things most likely to
             * blow it: a ~5 s blocking Wi-Fi scan, and a Wi-Fi association,
             * both on the same 2.4 GHz band the link is using, in rooms with
             * 14-19 visible APs. Every intermittent failure seen during setup
             * disconnected with reason 520 -- BLE_HS_HCI_ERR(0x08), connection
             * timeout -- which is exactly that budget running out.
             *
             * 6 s is the most Apple's peripheral guidelines allow, and the
             * request stays inside the rest of them: interval >= 15 ms, max >=
             * min + 15 ms, latency 0, and itvl_max * (latency + 1) * 3 well
             * under the timeout. A central is free to refuse, which is why the
             * result is logged rather than assumed -- see BLE_GAP_EVENT_CONN_UPDATE.
             */
            {
                struct ble_gap_upd_params p = {
                    .itvl_min            = 24,   /* 30 ms */
                    .itvl_max            = 40,   /* 50 ms */
                    .latency             = 0,
                    .supervision_timeout = 600,  /* 6 s */
                };
                int urc = ble_gap_update_params(s_conn, &p);
                if (urc) ESP_LOGI(TAG, "conn param request rejected (rc=%d)", urc);
            }
            rx_reset();
            if (s_on_connect) s_on_connect(s_user);
        } else {
            /* The controller stops advertising the moment it begins a
             * connection attempt, so a failed one leaves us not advertising
             * with the flag still set -- and start_advertising() opens by
             * returning early on exactly that flag. Clearing it first is what
             * makes this call do anything; without it the board goes silent
             * until someone reboots it. The disconnect and adv-complete paths
             * below already clear it for the same reason. */
            s_advertising = false;
            start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "central disconnected (reason %d)", event->disconnect.reason);
        log_ble_resources("on disconnect");
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_advertising = false;
        s_mtu = 0;
        rx_reset();
        if (s_want_adv) start_advertising();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        s_advertising = false;
        if (s_want_adv) start_advertising();
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE: {
        /* Whether the central took the timeout we asked for. It may refuse, or
         * grant something else; either way the number here is the one that
         * decides how much interference this link tolerates. */
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(event->conn_update.conn_handle, &d) == 0)
            ESP_LOGI(TAG, "conn params now: itvl %u (%u ms), latency %u, "
                           "timeout %u (%u ms) [status %d]",
                      d.conn_itvl, (unsigned)(d.conn_itvl * 125 / 100),
                      d.conn_latency, d.supervision_timeout,
                      (unsigned)(d.supervision_timeout * 10),
                      event->conn_update.status);
        return 0;
    }

    case BLE_GAP_EVENT_MTU:
        s_mtu = event->mtu.value;
        ESP_LOGI(TAG, "mtu now %d (chunk %u)", event->mtu.value,
                  (unsigned)chunk_len());
        return 0;

    default:
        return 0;
    }
}

static void start_advertising(void)
{
    /*
     * Each early return used to be silent, and the log then read "GATT
     * registered" followed by nothing -- indistinguishable from a host that
     * synced and advertised while the UART dropped the line. Say which gate
     * held, once per gate, so the log carries the reason.
     */
    static bool s_said_unsynced, s_said_busy;
    if (s_advertising) return;
    if (!s_host_synced) {
        if (!s_said_unsynced) { ESP_LOGI(TAG, "not advertising yet: host not synced"); s_said_unsynced = true; }
        return;
    }
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        if (!s_said_busy) { ESP_LOGI(TAG, "not advertising: a central holds the only slot (conn %u)", s_conn); s_said_busy = true; }
        return;
    }
    s_said_unsynced = s_said_busy = false;

    const char *serial = s_serial;
    const char *tail   = s_serial + strlen(s_serial) - 5;

    /* The 128-bit service UUID alone is 18 of the 31 advertising bytes, and the
     * name is another 20 -- they do not both fit. The name goes in the scan
     * response, which Web Bluetooth reads as part of the same advertisement. */
    uint8_t mfg[2 + 5];
    mfg[0] = MANUFACTURER_ID & 0xff;          /* company id is little-endian */
    mfg[1] = (MANUFACTURER_ID >> 8) & 0xff;
    memcpy(mfg + 2, tail, 5);

    struct ble_hs_adv_fields adv = {0};
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.uuids128 = (ble_uuid128_t *)&SVC_UUID;
    adv.num_uuids128 = 1;
    adv.uuids128_is_complete = 1;
    adv.mfg_data = mfg;
    adv.mfg_data_len = sizeof(mfg);

    struct ble_hs_adv_fields rsp = {0};
    rsp.name = (uint8_t *)serial;
    rsp.name_len = strlen(serial);
    rsp.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&adv);
    if (rc) { ESP_LOGI(TAG, "adv fields rejected (rc=%d)", rc); return; }
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc) { ESP_LOGI(TAG, "scan rsp rejected (rc=%d)", rc); return; }

    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        /* 100 ms, matching config.ADV_INTERVAL_MS on the MicroPython kit. */
        .itvl_min  = BLE_GAP_ADV_ITVL_MS(100),
        .itvl_max  = BLE_GAP_ADV_ITVL_MS(100),
    };
    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &params, gap_event, NULL);
    if (rc) { ESP_LOGI(TAG, "adv start failed (rc=%d)", rc); return; }
    s_advertising = true;
    ESP_LOGI(TAG, "advertising as '%s' (mfg %02x%02x/%.5s)", serial, mfg[1], mfg[0], tail);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    s_host_synced = true;
    /* The one line that separates "the host is up" from "the host never came
     * up": until it existed, both looked like GATT registered and silence. */
    ESP_LOGI(TAG, "host synced (ensure_addr rc=%d), want_adv=%d", rc, (int)s_want_adv);
    if (s_want_adv) start_advertising();
}

static void on_reset(int reason)
{
    ESP_LOGI(TAG, "host reset, reason %d", reason);
    s_host_synced = false;
    s_advertising = false;
    s_conn = BLE_HS_CONN_HANDLE_NONE;
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static bool s_stack_up;

esp_err_t buddy_ble_init(const char *serial, buddy_ble_data_cb_t on_data,
                         buddy_ble_connect_cb_t on_connect, void *user)
{
    if (!serial || strlen(serial) < 5 || strlen(serial) >= sizeof(s_serial)) return ESP_ERR_INVALID_ARG;
    snprintf(s_serial, sizeof(s_serial), "%s", serial);
    s_on_data = on_data;
    s_on_connect = on_connect;
    s_user = user;
    return ESP_OK;
}

static esp_err_t stack_up(void)
{
    if (s_stack_up) return ESP_OK;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(SVCS);
    if (rc) return ESP_FAIL;
    rc = ble_gatts_add_svcs(SVCS);
    if (rc) return ESP_FAIL;

    /* GAP name and the advertised name are the same string, so a central that
     * reads the name characteristic after connecting sees the serial too. */
    ble_svc_gap_device_name_set(s_serial);

    nimble_port_freertos_init(host_task);
    s_stack_up = true;
    ESP_LOGI(TAG, "GATT registered");
    /*
     * nimble_port_freertos_init() returns void: if the host task's stack
     * cannot be carved out of internal SRAM, the call fails silently and the
     * host never syncs -- the board sits in setup mode advertising nothing,
     * with a log that says only "GATT registered". Say whether the task exists,
     * and what the heap looked like, so that failure has a line of its own.
     */
    ESP_LOGI(TAG, "host task %s; internal heap %u B (largest %u)",
              xTaskGetHandle("nimble_host") ? "up" : "MISSING",
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
              (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}

esp_err_t buddy_ble_enable(void)
{
    esp_err_t err = stack_up();
    if (err != ESP_OK) return err;
    s_want_adv = true;
    start_advertising();
    return ESP_OK;
}

bool buddy_ble_is_up(void) { return s_stack_up; }

esp_err_t buddy_ble_shutdown(void)
{
    if (!s_stack_up) return ESP_OK;
    buddy_ble_disable();
    /* Let the disconnect and the last notification actually leave before the
     * controller goes away underneath them. */
    vTaskDelay(pdMS_TO_TICKS(200));

    int rc = nimble_port_stop();
    if (rc == 0) {
        nimble_port_deinit();
        s_stack_up = false;
        s_host_synced = false;
        ESP_LOGI(TAG, "controller down, memory released");
    } else {
        /* Not fatal, but say so loudly: the whole reason for this call is to
         * give the controller's internal RAM back to TLS and the voice path. */
        ESP_LOGI(TAG, "nimble_port_stop failed (rc=%d); BLE still up", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t buddy_ble_disable(void)
{
    s_want_adv = false;
    if (s_advertising) {
        ble_gap_adv_stop();
        s_advertising = false;
    }
    if (s_conn != BLE_HS_CONN_HANDLE_NONE)
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    ESP_LOGI(TAG, "advertising disabled");
    return ESP_OK;
}

bool buddy_ble_is_connected(void)   { return s_conn != BLE_HS_CONN_HANDLE_NONE; }
bool buddy_ble_is_advertising(void) { return s_advertising; }
