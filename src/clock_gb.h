// SPDX-License-Identifier: MPL-2.0
#ifndef SIGIL_CLOCK_GB_H
#define SIGIL_CLOCK_GB_H

#include "sigil_internal.h"

/**
 * An MBC3 real-time clock as the neutral form holds it: the current
 * registers, valid at the UTC time `stamp`. Latched registers are carried but
 * advisory, since a game latches again before it reads them.
 */
typedef struct {
    uint8_t  seconds, minutes, hours;
    uint16_t days;            /* 0-511: the low byte and DH bit 0 */
    bool     halted;          /* DH bit 6 */
    bool     carry;           /* DH bit 7: the day counter overflowed */
    uint8_t  latched[5];      /* S, M, H, DL, DH as last latched */
    int64_t  stamp;           /* Unix seconds, UTC */
} sigil_gb_clock;

/** The clock layouts emulators write. See docs/save-research/nintendo-cart.md section 2.1. */
typedef enum {
    SIGIL_GB_CLOCK_VBA,           /* 48 bytes: 10 u32 LE regs (current, latched), u64 LE time. VBA-M, SameBoy, Gearboy standalone, and the neutral form */
    SIGIL_GB_CLOCK_VBA32,         /* 44 bytes: the same with a u32 time */
    SIGIL_GB_CLOCK_MGBA,          /* 48 bytes in the VBA layout, but mGBA pairs the time with the latched registers */
    SIGIL_GB_CLOCK_GAMBATTE,      /* 8 bytes: u64 LE base time; the counter is now minus base */
    SIGIL_GB_CLOCK_MESEN2,        /* 13 bytes: 5 current regs, u64 BE milliseconds */
    SIGIL_GB_CLOCK_SAMEBOY_LIBRETRO, /* 32 bytes: 5 real, 5 latched regs, 6 pad, u64 LE last second, u32 cycles, 4 pad */
    SIGIL_GB_CLOCK_TGB_DUAL       /* 4 bytes: u32 LE emulated seconds, no wall time */
} sigil_gb_clock_format;

/** Bytes a clock takes in `format`, or 0 for an unknown format. */
size_t sigil_gb_clock_size(sigil_gb_clock_format format);

/**
 * Reads a clock written in `format`. `now` is the time a clock without wall
 * time (TGB Dual) is taken to be valid at. `*lossy` is set when the format
 * doesn't record something the neutral clock holds (halt and carry for
 * gambatte, latched registers for Mesen2, wall time for TGB Dual).
 * SIGIL_ERR_UNSUPPORTED_FORMAT when `len` doesn't match the format.
 */
int sigil_gb_clock_read(sigil_gb_clock_format format, const uint8_t *data, size_t len, int64_t now,
                        sigil_gb_clock *out, bool *lossy);

/**
 * Writes `clock` in `format` into `out`, which holds sigil_gb_clock_size bytes.
 * `*lossy` is set when the format can't keep part of the clock.
 */
int sigil_gb_clock_write(sigil_gb_clock_format format, const sigil_gb_clock *clock, uint8_t *out, bool *lossy);

#endif
