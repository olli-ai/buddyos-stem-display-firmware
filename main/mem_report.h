#pragma once
/**
 * The measurement this whole device type is gated on.
 *
 * The faces spec asks for free internal SRAM *with the radio up*, because that is
 * where this product line has been bitten before: on the robokit the BLE host task
 * failed to start with 1,351 B of internal SRAM left while PSRAM sat free. So every
 * figure here is reported per-capability, never as one "free heap" number, and the
 * one that matters most is the **minimum ever** — the low-water mark, not whatever
 * happens to be free when you look.
 */
void mem_report(const char *stage);
