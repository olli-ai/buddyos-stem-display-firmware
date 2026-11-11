# Setting a kit up

Getting a flashed board onto Wi-Fi and into an account, and how to undo that.

[All docs](README.md) · [Repository README](../README.md)

---

## From a flashed board to an account

Once the firmware is flashed, everything else happens at **<https://app.buddystem.com>** —
create an account, add the kit over Bluetooth (the setup screen shows its serial as a QR
to scan), design its face, give it a voice and choose what a touch does. A kit you built
yourself appears there exactly like one that came in a box.

## Setting a kit up

A kit that was never set up boots into **BLE setup** and advertises as its serial,
`ESP32-<factory MAC>`, with the manufacturer data (`0x4F4C` + the serial's last five
characters) that stem-fe's *Set up with Bluetooth* filters on. The GATT contract is
the other STEM kits', carried over into `components/buddy_ble` (which is in this repo and
is the complete implementation), so the website drives it unchanged:

1. `{"params":{"scan_wifi":1}}` → the kit scans and answers `wifi_list`.
2. `{"params":{ssid, password, user_jwt, mqtt_jwt, env, gateway}}` → the kit joins
   Wi-Fi (30 s budget) and answers `wifi_connection` — `successful`, or `failed`
   with `10002` wrong password / `10004` network not found / `10006` other.
3. On success it saves everything to NVS (`main/prov.c`), **shuts BLE down**, joins
   the broker and publishes `device.activate` (`device_type: buddyos_display_kit`)
   until the gateway answers `device.activate_status`. The website learns the
   outcome from the gateway, not from BLE (ADR 0006).

`gateway` is new with this kit: the stem-gateway host its voice socket dials
(`wss://<gateway>/v1/stem/voice/stream`), sent by stem-fe from
`VITE_STEM_DEVICE_GATEWAY_HOST`. Setup credentials always win over the bench ones in
`sdkconfig`, which are used only when nothing was provisioned.

**To set a kit up again, hold BOOT and let go.** Two seconds
(`CONFIG_BUDDY_BOOT_BUTTON_MS`) while the kit is running: it erases the setup and reboots
advertising, even on a bench build. It acts on the **release**, so letting go early is how
you change your mind. GPIO 0 is a button and nothing else on this board, so it can be
polled with the panel up — that was not true while this kit had an RGB panel, which is why
a second gesture exists: `CONFIG_BUDDY_BOOT_RESET` reads the pin once at boot, for a kit
woken from deep sleep with the button already down. Holding BOOT through a *cold reset*
lands in the ROM bootloader instead, and never reaches this firmware at all.

### Bringing it onto BuddyOS

The firmware is free; the cloud it talks to is a paid service, and that is the deal. Register
at the BuddyOS STEM website, then follow the **display-kit setup guide** in its *Documents*
section — it is written for people holding a board, kept in step with this repo, and available
in English and Vietnamese. It covers creating the account, provisioning over Bluetooth
and what cloud usage costs — including the talk button and the volume/barge-in trade.

Two things to know before you start:

- **A kit you built is not offered firmware updates over the air.** Your partition table may
  not match ours, and a brick you cannot recover is a bad first day. Update by reflashing.
- **A device token may only subscribe to its own directive topic.** Publishing there is the
  gateway's privilege.
