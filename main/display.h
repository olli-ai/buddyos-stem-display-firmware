#pragma once
#include <stdbool.h>

/** Turn the panel's output on or off.
 *
 * ST7701S DISPOFF + SLPIN over its configuration wires: the glass goes black and the
 * controller stops scanning. NOTE that the backlight is hardwired — LEDA on 5 V, LEDK
 * through resistors to GND — so the result is black but still faintly glowing until
 * the chip itself sleeps and the board's 5 V is the backlight's only load. A
 * production board wants the backlight on a GPIO; see the README.
 */
void display_set_on(bool on);
