/*
 * rg_wiimote.h - Wii extension controllers (Classic Controller and nunchuck)
 * as a retro-go gamepad source.
 *
 * The Adafruit Wii Nunchuck Breakout Adapter exposes the controller on a
 * plain I2C bus: address 0x52, level shifted, with pull-ups on the adapter.
 * The protocol is the standard Wii extension one, verified on the TinyTang
 * bench (its WII-001..WII-004 records) rather than assumed:
 *
 *   - A part wakes when the host writes 0x55 to register 0xF0 and then 0x00
 *     to 0xFB. Before that it answers 0xff for its data reads.
 *   - Six bytes at 0xFA identify the part. A Classic Controller reads
 *     00 00 A4 20 01 01, and a third-party receiver for the NES/SNES Classic
 *     reads the same but only reports usefully in data format 3.
 *   - A Classic is put in format 3 by writing 0x03 to 0xFE, then reports
 *     eight bytes at 0x00: four 8-bit axes, triggers, and two button bytes.
 *   - A nunchuck reports six bytes at 0x00 and needs no format write.
 *
 * The transaction shape is load-bearing and is NOT what rg_i2c_read() does:
 * the extension needs a write of the register, a STOP, a pause, then a
 * separate read. rg_i2c_read() issues a repeated START instead, which is why
 * this driver talks to the port directly.
 */
#ifndef RG_WIIMOTE_H
#define RG_WIIMOTE_H

#include <stdbool.h>
#include <stdint.h>

#define RG_WIIMOTE_ADDR 0x52

/* What the identity bytes at 0xFA said. */
typedef enum
{
    RG_WII_NONE = 0,
    RG_WII_NUNCHUCK,
    RG_WII_CLASSIC,
    RG_WII_UNKNOWN,
} rg_wii_kind_t;

/* Scan the bus and report what answered, for bring-up logging. */
void rg_wiimote_scan(void);

/* Wake and identify the part. Safe to call repeatedly. */
rg_wii_kind_t rg_wiimote_init(void);

/* Read one report and return a retro-go button mask, or false if the part is
 * absent, still asleep, or not identified. */
bool rg_wiimote_read(uint32_t *keys);

/* For the log: the kind detected and the last raw report. */
const char *rg_wiimote_kind_name(void);

#endif
