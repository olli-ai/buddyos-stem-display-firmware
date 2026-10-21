#pragma once
#include <stdbool.h>
/** Bring up Wi-Fi station, then MQTT. Both are asynchronous; mem_report() is called
 *  at each milestone, which is the point of having them here at all. */
void net_start(void);

/** Publish `device.sleeping` and wait up to `timeout_ms` for it to leave the device.
 *  Called from the sleep path, where the next thing to happen is the radio powering
 *  down — so a queued-but-unsent event would simply be lost. True if it was acked. */
bool net_publish_sleeping(const char *reason, int timeout_ms);

/** The factory-reset gesture: BOOT held for 5 s. Call first thing in app_main, before
 *  the panel takes GPIO 0 as a data line — it is the only moment BOOT can be read. Does
 *  not return if the reset happens. Returns at once if BOOT is not down. */
void net_boot_reset_check(void);
