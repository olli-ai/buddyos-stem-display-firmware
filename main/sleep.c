/* Sleep and wake. See sleep.h for the shape and why the button is GPIO 0. */
#include "sleep.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "display.h"
#include "face_assets.h"
#include "face_view.h"
#include "net.h"
#include "voice.h"

static const char *TAG = "sleep";

#if CONFIG_BUDDY_SLEEP

static volatile int64_t s_last_activity_ms;
static bool s_woke_from_deep;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

void sleep_note_activity(void) { s_last_activity_ms = now_ms(); }

bool sleep_woke_from_deep(void) { return s_woke_from_deep; }

/* The `asleep` state the website already authors — a Buddy closing its eyes reads as a
 * character going to sleep, where a screen that simply blanks reads as a fault. Skipped
 * silently if a design does not carry one. */
static void show_asleep_face(void)
{
    for (int i = 0; i < face_state_count; i++)
        if (!strcmp(face_states[i].name, "asleep")) {
            face_view_request_state(i);
            vTaskDelay(pdMS_TO_TICKS(CONFIG_BUDDY_SLEEP_FACE_MS));
            return;
        }
}

void sleep_now(const char *reason)
{
    if (!reason) reason = "manual";

    /* Never cut a sentence in half. A kit that sleeps mid-reply looks broken, and the
     * child's question was answered into a dead speaker. */
    for (int waited = 0; voice_active() && waited < 30000; waited += 250)
        vTaskDelay(pdMS_TO_TICKS(250));

    ESP_LOGI(TAG, "going to sleep (%s)", reason);
    show_asleep_face();

    /* Tell the gateway BEFORE the radio goes, and wait for the publish to actually
     * leave. This firmware already learned the lesson once, on the BLE provisioning
     * notification: an event that was queued but never sent is how a working kit
     * disappears from the website. */
    net_publish_sleeping(reason, 3000);

    display_set_on(false);

    const gpio_num_t pin = (gpio_num_t)CONFIG_BUDDY_SLEEP_WAKE_GPIO;

    /* Wake on the button going LOW (it idles high through its pull-up). If it is still
     * held from whatever triggered this, sleeping now would wake instantly — so wait for
     * the release, with a ceiling in case the pin is stuck. */
    for (int waited = 0; gpio_get_level(pin) == 0 && waited < 5000; waited += 50)
        vTaskDelay(pdMS_TO_TICKS(50));

    esp_err_t err = esp_sleep_enable_ext0_wakeup(pin, 0);
    if (err != ESP_OK) {
        /* Only reachable if somebody sets the wake pin outside GPIO 0-21: ext0 needs an
         * RTC GPIO. Staying awake is a far better failure than sleeping with no way back. */
        ESP_LOGE(TAG, "GPIO %d cannot wake from deep sleep (%s) — staying awake. "
                      "CONFIG_BUDDY_SLEEP_WAKE_GPIO must be an RTC GPIO, 0-21.",
                 (int)pin, esp_err_to_name(err));
        display_set_on(true);
        sleep_note_activity();
        return;
    }

    ESP_LOGI(TAG, "deep sleep now; press GPIO %d to wake", (int)pin);
    vTaskDelay(pdMS_TO_TICKS(50));           /* let the log line drain */
    esp_deep_sleep_start();                  /* does not return */
}

static void idle_task(void *arg)
{
    (void)arg;
    const int64_t limit_ms = (int64_t)CONFIG_BUDDY_SLEEP_IDLE_S * 1000;
    sleep_note_activity();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (voice_active()) { sleep_note_activity(); continue; }
        if (now_ms() - s_last_activity_ms >= limit_ms) sleep_now("idle");
    }
}

void sleep_init(void)
{
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    s_woke_from_deep = (cause == ESP_SLEEP_WAKEUP_EXT0);
    if (s_woke_from_deep)
        ESP_LOGI(TAG, "woke from deep sleep on GPIO %d", CONFIG_BUDDY_SLEEP_WAKE_GPIO);

    if (CONFIG_BUDDY_SLEEP_IDLE_S <= 0) {
        ESP_LOGI(TAG, "idle sleep disabled; only a directive can sleep this kit");
        sleep_note_activity();
        return;
    }
    ESP_LOGI(TAG, "idle sleep after %d s of quiet", CONFIG_BUDDY_SLEEP_IDLE_S);
    xTaskCreate(idle_task, "sleep_idle", 3072, NULL, 2, NULL);
}

#else  /* !CONFIG_BUDDY_SLEEP */

void sleep_init(void) {}
void sleep_note_activity(void) {}
void sleep_now(const char *reason) { (void)reason; }
bool sleep_woke_from_deep(void) { return false; }

#endif
