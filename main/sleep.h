#pragma once
#include <stdbool.h>
#include <stdint.h>

/**
 * Sleep: the kit goes quiet, and one button press brings it back.
 *
 * Spec: ../buddyos-stem-features-management/docs/features/display-kit-sleep-mode.md
 *
 * Deep sleep, woken by a press of BOOT (CONFIG_BUDDY_SLEEP_WAKE_GPIO, GPIO 0 by default).
 * Deep rather than light because the point is microamps; GPIO 0 rather than any other pin
 * because wake-from-deep needs an **RTC GPIO (0-21)** — ext0 refuses the rest — and GPIO 0
 * is the one button every ESP32-S3 devkit already has, and which this firmware already
 * reads for the 5-second factory reset.
 *
 * On the ST7701S board GPIO 0 is also DB17 of the panel bus, and every other RTC GPIO is
 * taken too (0-18 by the panel and audio, 19/20 by USB, 21 by PCLK). It still works as
 * the wake button because the panel is dark by then: sleep_now() takes the pin back from
 * the LCD peripheral before arming ext0. Awake, BOOT cannot be read at all.
 *
 * **Deep sleep has no microphone, so the wake word cannot wake the kit.** After
 * CONFIG_BUDDY_SLEEP_IDLE_S of quiet, "Jarvis" stops working until somebody presses the
 * button. That is the trade for the power, and it is the single most surprising thing
 * about this feature — it belongs in anything written for a reader.
 */

/** Read the wake cause and arm the idle timer. Call once, after the panel is up. */
void sleep_init(void);

/** Reset the idle countdown. Call on anything the kit does or is told: a wake word, a
 *  directive, a button, a conversation. */
void sleep_note_activity(void);

/** Begin the sleep sequence: let the Buddy finish talking, tell the gateway, blank the
 *  panel, then deep sleep. `reason` is carried in the event and is
 *  one of "idle", "voice", "blockly", "manual". Does not return if it succeeds. */
void sleep_now(const char *reason);

/** True when this boot was a wake from sleep rather than a cold start. */
bool sleep_woke_from_deep(void);

/** Milliseconds since the last sleep_note_activity(). Counts whether or not deep
 *  sleep is compiled in — the clock face uses the same idea of "left alone". */
int64_t sleep_idle_ms(void);
