/*
 * BuddyOS STEM — display firmware.
 *
 * Deliberately does nothing yet. The first real commit is panel bring-up, and the
 * panel has not been chosen (see README): its resolution, bus and shape decide the
 * draw buffers, the sprite sizes and the catalogue budget, so guessing here would
 * mean writing code that has to be thrown away.
 *
 * The order the planning docs argue for, and the reason for it:
 *
 *   1. panel + LVGL, one hard-coded face on the glass.
 *      GATE: measure free internal SRAM, free PSRAM and frame time with the radio up.
 *            This is where the memory question gets a number instead of an argument.
 *   2. the face renderer, composing catalogue sprites from stored choices.
 *   3. the `.buddy` motion interpreter over lv_anim, held to the fixtures in
 *      ../buddyos-stem-fe/src/data/motion.fixtures.json.
 *   4. the catalogue partition, delivered like the vision model already is.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "buddy-display";

void app_main(void)
{
    ESP_LOGI(TAG, "BuddyOS STEM display firmware — skeleton, no panel selected yet.");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
