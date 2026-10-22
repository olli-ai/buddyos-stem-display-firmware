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
 * conversation state arrives as JSON text frames: listening, thinking, speaking.
 *
 * Interruption works two ways, and the kit needs both. Saying the wake word over a
 * reply sends omni `{"type":"interrupt"}` and empties the playback queue on the spot —
 * the fast path, no round trip. omni also interrupts itself when its own speech
 * detection hears the child, which needs no wake word but only works while the
 * microphone is open during playback (CONFIG_BUDDY_AEC); it announces that with a
 * `flush_audio` status, and the queue is emptied on that exactly as it is on the wake
 * word. Without honouring it the Buddy talks over the child for most of a second
 * after omni has stopped.
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

/** Settle the face, or wake it.
 *
 * What a child's "bye bye" turns into: the Buddy wears `asleep` and stays that way
 * until the wake word starts a new conversation, rather than for as long as the room
 * happens to stay quiet. Deliberately NOT deep sleep — on this board that can only be
 * undone with the RESET button (see BUDDY_SLEEP_WAKE_GPIO), and a Buddy a child
 * cannot wake by speaking to it is not asleep, it is off. */
void voice_face_rest(bool rest);
