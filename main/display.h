#pragma once
#include <stdbool.h>

/** Turn the panel's output on or off.
 *
 * The controller's DISPOFF: the glass goes black and the controller stops scanning.
 * On the S3's GC9A01 the backlight is the module's own and stays lit, so the result is
 * black but faintly glowing; the P4 turns its backlight off as well (lcd_p4.c). A
 * production board wants the backlight on a GPIO.
 */
void display_set_on(bool on);
