/* Sleep and wake. See sleep.h for the shape and why the button is GPIO 0. */
#include "sleep.h"

#include "board_pins.h"
#include "driver/gpio.h"
#if CONFIG_IDF_TARGET_ESP32P4
#include "driver/rtc_io.h"
#endif
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "display.h"
#include "net.h"
#include "voice.h"

static const char *TAG = "sleep";

#if CONFIG_IDF_TARGET_ESP32P4
/* Let go of the C6's reset line, held low through deep sleep (sleep_now() below). This
 * cannot wait for app_main: esp_hosted starts the C6 from a constructor of its own,
 * and a numbered constructor runs before every unnumbered one. A no-op on a cold boot. */
static void __attribute__((constructor(101))) c6_reset_release(void)
{
    gpio_hold_dis(PIN_C6_RESET);
}
#endif

/* "Nothing has happened for a while" is asked by two features now — the clock that
 * replaces the face, and deep sleep — so the timestamp and its one-line reader live
 * outside CONFIG_BUDDY_SLEEP. They used to be inside it, which was fine while deep
 * sleep was the only thing that cared, and would have meant ten call sites quietly
 * doing nothing the moment someone turned it off. */
static volatile int64_t s_last_activity_ms;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

void sleep_note_activity(void) { s_last_activity_ms = now_ms(); }

int64_t sleep_idle_ms(void) { return now_ms() - s_last_activity_ms; }

#if CONFIG_BUDDY_SLEEP

static bool s_woke_from_deep;

bool sleep_woke_from_deep(void) { return s_woke_from_deep; }

void sleep_now(const char *reason)
{
    if (!reason) reason = "manual";

    /* Never cut a sentence in half. A kit that sleeps mid-reply looks broken, and the
     * child's question was answered into a dead speaker. */
    for (int waited = 0; voice_active() && waited < 30000; waited += 250)
        vTaskDelay(pdMS_TO_TICKS(250));

    const int pin = CONFIG_BUDDY_SLEEP_WAKE_GPIO;

#if CONFIG_IDF_TARGET_ESP32P4
    /* The Waveshare P4 board: no LP GPIO button, so by default no wake pin at all and
     * RESET wakes it — a cold boot, which is what a wake is anyway. See the Kconfig. */
    if (pin >= 0 && !esp_sleep_is_valid_wakeup_gpio((gpio_num_t)pin)) {
        ESP_LOGE(TAG, "GPIO %d cannot wake from deep sleep — staying awake. "
                      "CONFIG_BUDDY_SLEEP_WAKE_GPIO must be an LP GPIO, 0-15, or -1.", pin);
        sleep_note_activity();
        return;
    }
    ESP_LOGI(TAG, "going to sleep (%s)", reason);
    net_publish_sleeping(reason, 3000);
    display_set_on(false);

    /* A preprocessor test, not `if (pin >= 0)`: with the default -1 the shift below
     * would still be compiled, and a negative shift count is a warning even when dead. */
#if CONFIG_BUDDY_SLEEP_WAKE_GPIO >= 0
    {
        gpio_reset_pin((gpio_num_t)pin);
        for (int waited = 0; gpio_get_level((gpio_num_t)pin) == 0 && waited < 5000; waited += 50)
            vTaskDelay(pdMS_TO_TICKS(50));
        rtc_gpio_pullup_en((gpio_num_t)pin);
        rtc_gpio_pulldown_dis((gpio_num_t)pin);
        esp_err_t err = esp_sleep_enable_ext1_wakeup_io(1ULL << pin, ESP_EXT1_WAKEUP_ANY_LOW);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ext1 on GPIO %d refused (%s) — restarting rather than sleeping blind",
                     pin, esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(50));
            esp_restart();
        }
    }
#endif

    /* The radio is a whole second chip. Left alone the C6 stays up, associated and
     * drawing tens of milliamps beside a P4 in microamps; held in reset it draws
     * almost nothing. The hold survives deep sleep and is let go at boot (below),
     * before esp_hosted pulses this same line to start the C6. */
    gpio_set_direction(PIN_C6_RESET, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_C6_RESET, 0);
    gpio_hold_en(PIN_C6_RESET);

    if (pin >= 0) ESP_LOGI(TAG, "deep sleep now; press GPIO %d or RESET to wake", pin);
    else ESP_LOGI(TAG, "deep sleep now; press RESET to wake");
    vTaskDelay(pdMS_TO_TICKS(50));           /* let the log line drain */
    esp_deep_sleep_start();                  /* does not return */
#else
    /* Checked before anything is torn down, because past the next few lines the wake
     * pin may no longer be a panel data line and there is no putting it back. Only
     * reachable if somebody sets the wake pin outside GPIO 0-21: ext0 needs an RTC
     * GPIO. Staying awake is a far better failure than sleeping with no way back. */
    if (!esp_sleep_is_valid_wakeup_gpio(pin)) {
        ESP_LOGE(TAG, "GPIO %d cannot wake from deep sleep — staying awake. "
                      "CONFIG_BUDDY_SLEEP_WAKE_GPIO must be an RTC GPIO, 0-21.", (int)pin);
        sleep_note_activity();
        return;
    }

    ESP_LOGI(TAG, "going to sleep (%s)", reason);

    /* Tell the gateway BEFORE the radio goes, and wait for the publish to actually
     * leave. This firmware already learned the lesson once, on the BLE provisioning
     * notification: an event that was queued but never sent is how a working kit
     * disappears from the website. */
    net_publish_sleeping(reason, 3000);

    display_set_on(false);

    /* The wake pin to a plain input with its pull-up, ready for ext0, whatever it was
     * configured as before. */
    gpio_reset_pin(pin);

    /* Wake on the button going LOW (it idles high through its pull-up). If it is still
     * held from whatever triggered this, sleeping now would wake instantly — so wait for
     * the release, with a ceiling in case the pin is stuck. */
    for (int waited = 0; gpio_get_level(pin) == 0 && waited < 5000; waited += 50)
        vTaskDelay(pdMS_TO_TICKS(50));

    esp_err_t err = esp_sleep_enable_ext0_wakeup(pin, 0);
    if (err != ESP_OK) {
        /* The pin was checked above, so this is not supposed to happen. The panel has
         * lost a data line and cannot be given it back from here: a restart is the
         * clean way to stay awake. */
        ESP_LOGE(TAG, "ext0 on GPIO %d refused (%s) — restarting rather than sleeping blind",
                 (int)pin, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_restart();
    }

    ESP_LOGI(TAG, "deep sleep now; press GPIO %d to wake", (int)pin);
    vTaskDelay(pdMS_TO_TICKS(50));           /* let the log line drain */
    esp_deep_sleep_start();                  /* does not return */
#endif
}

static void idle_task(void *arg)
{
    (void)arg;
    const int64_t limit_ms = (int64_t)CONFIG_BUDDY_SLEEP_IDLE_S * 1000;
    sleep_note_activity();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (voice_active()) { sleep_note_activity(); continue; }
        if (sleep_idle_ms() >= limit_ms) sleep_now("idle");
    }
}

void sleep_init(void)
{
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    s_woke_from_deep = (cause == ESP_SLEEP_WAKEUP_EXT0 || cause == ESP_SLEEP_WAKEUP_EXT1);
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
void sleep_now(const char *reason) { (void)reason; }
bool sleep_woke_from_deep(void) { return false; }

#endif
