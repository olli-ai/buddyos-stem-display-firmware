#pragma once
#include <stdbool.h>

/**
 * Talk to the Buddy: the kit's end of the voice path.
 *
 *     mic 16 kHz -> DC block -> x3 FIR -> Opus 48 kHz/20 ms -> wss -> gateway -> omni
 *     amp 16 kHz <- Opus decode       <-                     wss <-
 *
 * One raw Opus packet per binary WebSocket message, in both directions, because that
 * is exactly what omni's proto asks for and what the gateway relays unchanged. omni's
 * conversation state arrives as JSON text frames and drives the face: listening,
 * thinking, speaking.
 *
 * Needs Wi-Fi up. With CONFIG_BUDDY_WAKE_WORD (the default) nothing is sent until
 * WakeNet hears the wake word on the device; the conversation then runs until
 * CONFIG_BUDDY_VOICE_IDLE_S of quiet and the socket closes again. Without it, the
 * session opens at boot and reconnects on its own.
 */
/** `gateway` is the stem-gateway host (no scheme, no path) and `user_token` the
 *  owner's JWT, both from setup (main/prov.h). The URL is the gateway's
 *  wss://<gateway>/v1/stem/voice/stream. */
bool voice_start(const char *serial, const char *gateway, const char *user_token);

/** True while a session is connected — the demo loops stand aside for a real one. */
bool voice_active(void);
