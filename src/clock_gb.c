// SPDX-License-Identifier: MPL-2.0
#include "clock_gb.h"

#define DH_DAY_HIGH 0x01u
#define DH_HALT     0x40u
#define DH_CARRY    0x80u
#define DAY_LIMIT   512u
#define DAY_SECONDS 86400

static uint8_t dh_of(const sigil_gb_clock *c) {
    return (uint8_t)(((c->days >> 8) & DH_DAY_HIGH) | (c->halted ? DH_HALT : 0) | (c->carry ? DH_CARRY : 0));
}

/* Sets the current registers from S, M, H, DL, DH. VBA-M can hold the day
 * count unmasked in DL, so a DL above 255 is taken as the whole count. */
static void set_registers(sigil_gb_clock *c, uint32_t s, uint32_t m, uint32_t h, uint32_t dl, uint32_t dh) {
    c->seconds = (uint8_t)(s % 60);
    c->minutes = (uint8_t)(m % 60);
    c->hours = (uint8_t)(h % 24);
    c->days = (uint16_t)(dl > 0xFF ? dl % DAY_LIMIT : (dl | ((dh & DH_DAY_HIGH) << 8)));
    c->halted = (dh & DH_HALT) != 0;
    c->carry = (dh & DH_CARRY) != 0;
}

static void set_from_seconds(sigil_gb_clock *c, uint64_t total) {
    uint64_t days = total / DAY_SECONDS;
    c->seconds = (uint8_t)(total % 60);
    c->minutes = (uint8_t)(total / 60 % 60);
    c->hours = (uint8_t)(total / 3600 % 24);
    c->carry = days >= DAY_LIMIT;
    c->days = (uint16_t)(days % DAY_LIMIT);
    c->halted = false;
}

static int64_t seconds_of(const sigil_gb_clock *c) {
    return (int64_t)c->days * DAY_SECONDS + (int64_t)c->hours * 3600 + (int64_t)c->minutes * 60 + c->seconds;
}

static void latch_current(sigil_gb_clock *c) {
    c->latched[0] = c->seconds;
    c->latched[1] = c->minutes;
    c->latched[2] = c->hours;
    c->latched[3] = (uint8_t)c->days;
    c->latched[4] = dh_of(c);
}

static bool latched_matches_current(const sigil_gb_clock *c) {
    sigil_gb_clock copy = *c;
    latch_current(&copy);
    return memcmp(copy.latched, c->latched, sizeof(c->latched)) == 0;
}

size_t sigil_gb_clock_size(sigil_gb_clock_format format) {
    switch (format) {
    case SIGIL_GB_CLOCK_VBA:              return 48;
    case SIGIL_GB_CLOCK_VBA32:            return 44;
    case SIGIL_GB_CLOCK_MGBA:             return 48;
    case SIGIL_GB_CLOCK_GAMBATTE:         return 8;
    case SIGIL_GB_CLOCK_MESEN2:           return 13;
    case SIGIL_GB_CLOCK_SAMEBOY_LIBRETRO: return 32;
    case SIGIL_GB_CLOCK_TGB_DUAL:         return 4;
    }
    return 0;
}

int sigil_gb_clock_read(sigil_gb_clock_format format, const uint8_t *data, size_t len, int64_t now,
                        sigil_gb_clock *out, bool *lossy) {
    if (!data || !out || !lossy) return SIGIL_ERR_INVALID_ARG;
    size_t size = sigil_gb_clock_size(format);
    if (size == 0 || len != size) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    memset(out, 0, sizeof(*out));
    *lossy = false;

    switch (format) {
    case SIGIL_GB_CLOCK_VBA:
    case SIGIL_GB_CLOCK_VBA32:
        set_registers(out, sigil_read_le32(data), sigil_read_le32(data + 4), sigil_read_le32(data + 8), sigil_read_le32(data + 12), sigil_read_le32(data + 16));
        for (int i = 0; i < 5; i++) out->latched[i] = (uint8_t)sigil_read_le32(data + 20 + 4 * i);
        out->stamp = format == SIGIL_GB_CLOCK_VBA ? (int64_t)sigil_read_le64(data + 40) : (int64_t)sigil_read_le32(data + 40);
        return SIGIL_OK;
    case SIGIL_GB_CLOCK_MGBA:
        set_registers(out, sigil_read_le32(data + 20), sigil_read_le32(data + 24), sigil_read_le32(data + 28), sigil_read_le32(data + 32), sigil_read_le32(data + 36));
        for (int i = 0; i < 5; i++) out->latched[i] = (uint8_t)sigil_read_le32(data + 20 + 4 * i);
        out->stamp = (int64_t)sigil_read_le64(data + 40);
        return SIGIL_OK;
    case SIGIL_GB_CLOCK_GAMBATTE:
        out->stamp = (int64_t)sigil_read_le64(data);
        latch_current(out);
        *lossy = true;
        return SIGIL_OK;
    case SIGIL_GB_CLOCK_MESEN2:
        set_registers(out, data[0], data[1], data[2], data[3], data[4]);
        out->stamp = (int64_t)(sigil_read_be64(data + 5) / 1000);
        latch_current(out);
        *lossy = true;
        return SIGIL_OK;
    case SIGIL_GB_CLOCK_SAMEBOY_LIBRETRO:
        set_registers(out, data[0], data[1], data[2], data[3], data[4]);
        memcpy(out->latched, data + 5, 5);
        out->stamp = (int64_t)sigil_read_le64(data + 16);
        return SIGIL_OK;
    case SIGIL_GB_CLOCK_TGB_DUAL:
        set_from_seconds(out, sigil_read_le32(data));
        out->stamp = now;
        latch_current(out);
        *lossy = true;
        return SIGIL_OK;
    }
    return SIGIL_ERR_UNSUPPORTED_FORMAT;
}

static void put_vba_regs(uint8_t *out, const sigil_gb_clock *c, const uint8_t latched[5]) {
    sigil_write_le32(out, c->seconds);
    sigil_write_le32(out + 4, c->minutes);
    sigil_write_le32(out + 8, c->hours);
    sigil_write_le32(out + 12, (uint8_t)c->days);
    sigil_write_le32(out + 16, dh_of(c));
    for (int i = 0; i < 5; i++) sigil_write_le32(out + 20 + 4 * i, latched[i]);
}

int sigil_gb_clock_write(sigil_gb_clock_format format, const sigil_gb_clock *clock, uint8_t *out, bool *lossy) {
    if (!clock || !out || !lossy) return SIGIL_ERR_INVALID_ARG;
    size_t size = sigil_gb_clock_size(format);
    if (size == 0) return SIGIL_ERR_UNSUPPORTED_FORMAT;
    memset(out, 0, size);
    *lossy = false;

    sigil_gb_clock current = *clock;
    latch_current(&current);
    switch (format) {
    case SIGIL_GB_CLOCK_VBA:
        put_vba_regs(out, clock, clock->latched);
        sigil_write_le64(out + 40, (uint64_t)clock->stamp);
        return SIGIL_OK;
    case SIGIL_GB_CLOCK_VBA32:
        put_vba_regs(out, clock, clock->latched);
        sigil_write_le32(out + 40, (uint32_t)clock->stamp);
        *lossy = clock->stamp < 0 || clock->stamp > (int64_t)UINT32_MAX;
        return SIGIL_OK;
    case SIGIL_GB_CLOCK_MGBA:
        put_vba_regs(out, clock, current.latched);
        sigil_write_le64(out + 40, (uint64_t)clock->stamp);
        return SIGIL_OK;
    case SIGIL_GB_CLOCK_GAMBATTE: {
        int64_t elapsed = seconds_of(clock) + (clock->carry ? (int64_t)DAY_LIMIT * DAY_SECONDS : 0);
        sigil_write_le64(out, (uint64_t)(clock->stamp - elapsed));
        *lossy = clock->halted || clock->carry;
        return SIGIL_OK;
    }
    case SIGIL_GB_CLOCK_MESEN2:
        out[0] = clock->seconds;
        out[1] = clock->minutes;
        out[2] = clock->hours;
        out[3] = (uint8_t)clock->days;
        out[4] = dh_of(clock);
        sigil_write_be64(out + 5, (uint64_t)clock->stamp * 1000u);
        *lossy = !latched_matches_current(clock);
        return SIGIL_OK;
    case SIGIL_GB_CLOCK_SAMEBOY_LIBRETRO:
        out[0] = clock->seconds;
        out[1] = clock->minutes;
        out[2] = clock->hours;
        out[3] = (uint8_t)clock->days;
        out[4] = dh_of(clock);
        memcpy(out + 5, clock->latched, 5);
        sigil_write_le64(out + 16, (uint64_t)clock->stamp);
        return SIGIL_OK;
    case SIGIL_GB_CLOCK_TGB_DUAL:
        sigil_write_le32(out, (uint32_t)seconds_of(clock));
        *lossy = true;
        return SIGIL_OK;
    }
    return SIGIL_ERR_UNSUPPORTED_FORMAT;
}
