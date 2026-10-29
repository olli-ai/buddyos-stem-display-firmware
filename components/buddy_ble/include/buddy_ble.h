/* Copied from ../buddyos-stem-c-firmware/components/stem_ble (13e9490); see buddy_ble.c. */
#pragma once
/*
 * BLE setup service -- the same GATT contract the MicroPython kit exposes, so
 * the existing stem-fe setup flow drives this firmware unchanged.
 *
 * Fixed by that contract, do not "clean up":
 *   service  1a9e1774-8b71-11ef-a8cc-b25737277ce0
 *   setup    276fb4e4-8b71-11ef-a8cc-b25737277ce0   (write + notify)
 *   adv name = serial number, e.g. "ESP32-aabbccddeeff"
 *   manufacturer data = 0x4F4C followed by the last 5 hex of the serial
 *
 * The browser filters on that company id (SetupWithBluetooth.vue), so it is the
 * one field that decides whether the device is discoverable at all.
 *
 * Messages are JSON, reassembled on a closing '}'. Inbound chunking is the
 * app's business. Outbound, the chunk is the negotiated ATT_MTU minus the
 * 3-byte notification header, falling back to 16 bytes before the MTU exchange
 * -- 16 being what the app's reassembly was originally written against, and
 * still the only size the spec guarantees on a fresh link. The app accumulates
 * whatever arrives and parses when the JSON closes, so it does not care how the
 * bytes are divided; the device does, because 650 bytes at 16 a time is 41
 * notifications and most of a second with the Wi-Fi radio contending.
 */
#include <stdbool.h>
#include "esp_err.h"

typedef void (*buddy_ble_data_cb_t)(const char *json, void *user);
typedef void (*buddy_ble_connect_cb_t)(void *user);

/* Records the serial (advertised name; its last 5 characters are the manufacturer
 * data the app filters on) and the callbacks. The controller is NOT started here --
 * see buddy_ble_enable(). */
esp_err_t buddy_ble_init(const char *serial, buddy_ble_data_cb_t on_data,
                         buddy_ble_connect_cb_t on_connect, void *user);

/* Starts the stack if it is not already up, then advertises. Bringing the
 * controller up lazily matters: a provisioned device never needs BLE, and the
 * radio it does not start is ~60 KB of internal RAM it keeps. */
esp_err_t buddy_ble_enable(void);

/* Stops advertising and drops the connection; the stack stays up. */
esp_err_t buddy_ble_disable(void);

/*
 * Stops advertising AND tears the controller down, releasing its memory.
 *
 * Called once Wi-Fi is confirmed. The controller's internal RAM is what the
 * broker's and the voice socket's TLS handshakes want, and a provisioned kit has
 * no further use for the radio. A device that needs BLE again reboots into setup.
 */
esp_err_t buddy_ble_shutdown(void);
/*
 * There is deliberately no "release the controller memory" call here.
 *
 * esp_bt_controller_mem_release() on this board corrupts the heap -- measured
 * at 1 crash in 6 boots, invisible under heap poisoning, and surfacing far from
 * the cause.
 */

bool buddy_ble_is_connected(void);
bool buddy_ble_is_advertising(void);
/* Whether the controller exists at all. Distinct from the two above: after
 * buddy_ble_shutdown() there is nothing to advertise with and nothing to
 * reconnect to, so a caller that would otherwise wait for a central -- or
 * restart advertising to attract one -- has to know not to. */
bool buddy_ble_is_up(void);

/* Chunked notification. Blocking, and slow by design: a Wi-Fi scan list is
 * kilobytes and the controller drops notifications queued faster than it can
 * transmit them. */
esp_err_t buddy_ble_write(const char *json);
