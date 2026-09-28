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
 * Needs Wi-Fi up. Runs until the device resets; the WebSocket client reconnects on
 * its own.
 */
bool voice_start(const char *serial);

/** True while a session is connected — the demo loops stand aside for a real one. */
bool voice_active(void);
