# Left alone: the clock, and deep sleep

What the kit does when nobody is there, and why idle no longer ends in deep sleep.

[All docs](README.md) · [Repository README](../README.md)

---

### Being left alone: the clock

After **three minutes of quiet** (`CONFIG_BUDDY_CLOCK_IDLE_S`) the face gives way to the
time and the date, and the kit **stays awake and keeps listening**. "Jarvis" puts the face
back on both boards; on the P4 the first touch does too, and touching the screen counts as
somebody being there. The S3 kit has no touch, so on it the wake word is the way back —
which is why its build has voice on.

The time comes from SNTP once the kit has an address, in the zone named by
`CONFIG_BUDDY_TZ` — `ICT-7` is UTC+7, and note that POSIX inverts the sign. A kit that has
never been online keeps its face instead, because a clock reading 00:00 is worse than no
clock.

### Deep sleep, and why idle no longer reaches it

`CONFIG_BUDDY_SLEEP_IDLE_S` now defaults to **0**. Idle used to blank the panel and enter
deep sleep, which made a kit left on a shelf look broken, and on the P4 it could only be
undone with the RESET button — BOOT there is GPIO 35, not an LP GPIO, so it cannot wake
the chip. Being left alone meant needing an adult.

What remains is the explicit `device.sleep` directive, for someone who asked. **While it
is asleep, "Jarvis" does nothing** — deep sleep has no microphone, so the button is the
only way back, and on a board whose button cannot wake it there is no way back at all.
Read `CONFIG_BUDDY_SLEEP_WAKE_GPIO` before turning the idle timer back on.

Waking is a **cold boot**: RAM, Wi-Fi, TLS and the MQTT session are all gone. The screen
is drawn before the radio starts, so the face is back long before the network is.

Two details that are deliberate rather than incidental:

- **The wake pin must be an RTC GPIO (0–21).** Waking from deep sleep uses `ext0`, which
  the S3 supports on those pins only — `SOC_RTCIO_PIN_COUNT` is 22. GPIO 47 cannot do it at
  any speed, which is why the button is BOOT. A production enclosure's button has the same
  constraint. `CONFIG_BUDDY_SLEEP_WAKE_GPIO` is range-limited to 0–21 so this cannot be got
  wrong quietly, and `sleep_now()` refuses to sleep rather than sleep with no way back.
- **`device.sleeping` is published and waited for** before the radio goes down. An event
  that was queued but never sent is how a kit that works perfectly disappears from the
  website — the same lesson this firmware already learned on the BLE provisioning
  notification.

**Not yet confirmed on hardware:** GPIO 0 is a strapping pin, and held low at reset it
selects download mode. A press-and-release to wake should be clear of the latch, but
nobody has checked. It is a minute to test: let the kit sleep, press BOOT, and see whether
it boots or lands in the bootloader.

**There is a fallback pin.** On the S3 kit the screen and the audio use only GPIO 4–8 and
15–18 of the RTC GPIOs (0–21), so if BOOT turns out not to work as a wake pin, GPIO 21 is
free: wire a button to it and set `CONFIG_BUDDY_SLEEP_WAKE_GPIO=21`.
