#pragma once
#include <stdbool.h>

/** Turn the panel's output on or off.
 *
 * GC9A01 display-off over SPI: the controller stops driving pixels and the glass goes
 * black. NOTE that on a module whose backlight is hardwired — which the bring-up board's
 * is — the backlight stays lit, so the result is black but still glowing. A production
 * board wants the backlight on a GPIO; see the README.
 */
void display_set_on(bool on);
