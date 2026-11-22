// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"

/* No-Intro / Redump community naming patterns:
 *   PSP / PSVita: [ULUS10064] / (ULUS10064) / leading ULUS10064
 *   PS3:          the PSP pattern
 *   PSX / PS2:    [SLUS-12345] / (SLUS-12345) / bare LLLL-NNNNN
 *   Switch / 3DS: [0100ABCD12345000] / suffix [-_]<16hex>$
 *   Wii U:        [00050000<8 hex>] full id; canonical = last 8
 *   Wii / GC:     [RZTE] ASCII gameId; canonical = hex */

static void stem_of(const char *filename_hint, char out[256]) {
    out[0] = '\0';
    if (!filename_hint) return;

    const char *base = filename_hint;
    const char *slash = strrchr(filename_hint, '/');
    if (slash) base = slash + 1;
#ifdef _WIN32
    const char *bs = strrchr(filename_hint, '\\');
    if (bs && bs >= base) base = bs + 1;
#endif

    size_t len = strlen(base);
    const char *dot = strrchr(base, '.');
    if (dot && dot > base) len = (size_t)(dot - base);
    if (len > 255) len = 255;
    memcpy(out, base, len);
    out[len] = '\0';
}

static bool match_psp_id(const char *s, size_t len, size_t i, char out[10]) {
    if (i + 9 > len) return false;
    for (int k = 0; k < 4; k++) if (!sigil_is_upper(s[i + k])) return false;
    for (int k = 4; k < 9; k++) if (!sigil_is_dig(s[i + k])) return false;
    memcpy(out, s + i, 9);
    out[9] = '\0';
    return true;
}

/* PSP and Vita ids share the shape match_psp_id reads. Vita3K treats an id
 * starting with "PCS" as a retail Vita game (get_license in
 * vita3k/packages/src/license.cpp at 54aefe5). No PSP id starts with it:
 * none of the 7362 in libretro-database's two PSP databases at 90601bf
 * (rdb/Sony - PlayStation Portable.rdb and its PSN one), against 2497 of the
 * 2499 in its Vita database. */
static bool is_vita_retail_id(const char *id) {
    return strncmp(id, "PCS", 3) == 0;
}

static bool match_dashed_serial(const char *s, size_t len, size_t i, char out[11]) {
    if (i + 10 > len) return false;
    for (int k = 0; k < 4; k++) if (!sigil_is_upper(s[i + k])) return false;
    if (s[i + 4] != '-') return false;
    for (int k = 5; k < 10; k++) if (!sigil_is_dig(s[i + k])) return false;
    memcpy(out, s + i, 10);
    out[10] = '\0';
    return true;
}

static bool match_hex_run(const char *s, size_t len, size_t i, size_t n, char *out) {
    if (i + n > len) return false;
    for (size_t k = 0; k < n; k++) if (!sigil_is_hex(s[i + k])) return false;
    for (size_t k = 0; k < n; k++) out[k] = sigil_to_upper(s[i + k]);
    out[n] = '\0';
    return true;
}

static int try_psp(const char *stem, size_t len, sigil_result *out) {
    char id[10];
    for (size_t i = 0; i + 11 <= len; i++) {
        if (stem[i] == '[' && match_psp_id(stem, len, i + 1, id) && stem[i + 10] == ']') {
            memcpy(out->title_id, id, 10);
            memcpy(out->raw_serial, id, 10);
            return SIGIL_OK;
        }
    }
    for (size_t i = 0; i + 11 <= len; i++) {
        if (stem[i] == '(' && match_psp_id(stem, len, i + 1, id) && stem[i + 10] == ')') {
            memcpy(out->title_id, id, 10);
            memcpy(out->raw_serial, id, 10);
            return SIGIL_OK;
        }
    }
    if (match_psp_id(stem, len, 0, id)) {
        memcpy(out->title_id, id, 10);
        memcpy(out->raw_serial, id, 10);
        return SIGIL_OK;
    }
    return SIGIL_ERR_NOT_FOUND;
}

static int try_dashed_serial(const char *stem, size_t len, sigil_result *out) {
    char id[11];
    for (size_t i = 0; i + 12 <= len; i++) {
        if (stem[i] == '[' && match_dashed_serial(stem, len, i + 1, id) && stem[i + 11] == ']') {
            memcpy(out->title_id, id, 11);
            memcpy(out->raw_serial, id, 11);
            return SIGIL_OK;
        }
    }
    for (size_t i = 0; i + 12 <= len; i++) {
        if (stem[i] == '(' && match_dashed_serial(stem, len, i + 1, id) && stem[i + 11] == ')') {
            memcpy(out->title_id, id, 11);
            memcpy(out->raw_serial, id, 11);
            return SIGIL_OK;
        }
    }
    for (size_t i = 0; i + 10 <= len; i++) {
        if (match_dashed_serial(stem, len, i, id)) {
            memcpy(out->title_id, id, 11);
            memcpy(out->raw_serial, id, 11);
            return SIGIL_OK;
        }
    }
    return SIGIL_ERR_NOT_FOUND;
}

static int try_hex16(const char *stem, size_t len, sigil_result *out, bool require_01_prefix) {
    char id[17];
    for (size_t i = 0; i + 18 <= len; i++) {
        if (stem[i] == '[' && match_hex_run(stem, len, i + 1, 16, id) && stem[i + 17] == ']') {
            if (require_01_prefix && !(id[0] == '0' && id[1] == '1')) continue;
            memcpy(out->title_id, id, 17);
            memcpy(out->raw_serial, id, 17);
            return SIGIL_OK;
        }
    }
    for (size_t i = 0; i + 18 <= len; i++) {
        if (stem[i] == '(' && match_hex_run(stem, len, i + 1, 16, id) && stem[i + 17] == ')') {
            if (require_01_prefix && !(id[0] == '0' && id[1] == '1')) continue;
            memcpy(out->title_id, id, 17);
            memcpy(out->raw_serial, id, 17);
            return SIGIL_OK;
        }
    }
    if (require_01_prefix && len >= 17) {
        size_t i = len - 16;
        if (i > 0 && (stem[i - 1] == '-' || stem[i - 1] == '_')
            && match_hex_run(stem, len, i, 16, id)
            && id[0] == '0' && id[1] == '1') {
            memcpy(out->title_id, id, 17);
            memcpy(out->raw_serial, id, 17);
            return SIGIL_OK;
        }
    }
    return SIGIL_ERR_NOT_FOUND;
}

/* Only the 16-hex bracket carries both halves of the title id, so only it can
 * emit the save location threeds.c does for the same ROM: the '/'-split,
 * lowercase path azahar creates ("{:08x}", am.cpp:1324).
 *
 * An 8-hex bracket is the low half alone. It identifies the game, but it
 * addresses no save directory, and the high half cannot be inferred from it -
 * retail titles are 0004xxxx, not a fixed 00040000, so updates (0004000E) and
 * demos (00040002) would land in the wrong tree. Those branches therefore
 * leave save_id empty, the documented "unknown" value, rather than a flat id a
 * consumer would build a wrong directory from. `usage` describes the
 * platform's save layout, not how complete one filename is, so it stays
 * folder-split on every branch. */
static int try_3ds(const char *stem, size_t len, sigil_result *out) {
    char id16[17];
    for (size_t i = 0; i + 18 <= len; i++) {
        if (stem[i] == '[' && match_hex_run(stem, len, i + 1, 16, id16) && stem[i + 17] == ']') {
            if (id16[0] == '0' && id16[1] == '0' && id16[2] == '0' && id16[3] == '4') {
                memcpy(out->title_id, id16, 17);
                memcpy(out->raw_serial, id16, 17);
                sigil_hex16_split_lower(id16, out->save_id);
                return SIGIL_OK;
            }
        }
    }
    char id8[9];
    for (size_t i = 0; i + 10 <= len; i++) {
        if (stem[i] == '[' && match_hex_run(stem, len, i + 1, 8, id8) && stem[i + 9] == ']') {
            memcpy(out->title_id, id8, 9);
            memcpy(out->raw_serial, id8, 9);
            return SIGIL_OK;
        }
    }
    for (size_t i = 0; i + 10 <= len; i++) {
        if (stem[i] == '(' && match_hex_run(stem, len, i + 1, 8, id8) && stem[i + 9] == ')') {
            memcpy(out->title_id, id8, 9);
            memcpy(out->raw_serial, id8, 9);
            return SIGIL_OK;
        }
    }
    return SIGIL_ERR_NOT_FOUND;
}

/* A full id's save category, as Cemu's usr/save/<high>/ names it: a demo
 * (00050002) saves under its own, a game, update or DLC under the game's
 * 00050000. */
static const char *wiiu_save_category(const char id16[17]) {
    return strncmp(id16, "00050002", 8) == 0 ? "00050002" : "00050000";
}

static int try_wiiu(const char *stem, size_t len, sigil_result *out) {
    /* Filenames carry the FULL 16-hex; canonical save form is the LAST 8. A
     * bare 8 hex has no category, so save_id is the code alone. */
    char id16[17];
    for (size_t i = 0; i + 18 <= len; i++) {
        if (stem[i] == '[' && match_hex_run(stem, len, i + 1, 16, id16) && stem[i + 17] == ']') {
            if (id16[0] == '0' && id16[1] == '0' && id16[2] == '0' && id16[3] == '5') {
                memcpy(out->raw_serial, id16, 17);
                memcpy(out->title_id, id16 + 8, 9);
                char joined[18];
                snprintf(joined, sizeof(joined), "%s/%s", wiiu_save_category(id16), out->title_id);
                sigil_lower_copy(joined, out->save_id, sizeof(out->save_id));
                out->usage = SIGIL_USAGE_FOLDER_SPLIT;
                return SIGIL_OK;
            }
        }
    }
    char id8[9];
    for (size_t i = 0; i + 10 <= len; i++) {
        if (stem[i] == '[' && match_hex_run(stem, len, i + 1, 8, id8) && stem[i + 9] == ']') {
            memcpy(out->title_id, id8, 9);
            memcpy(out->raw_serial, id8, 9);
            sigil_lower_copy(out->title_id, out->save_id, sizeof(out->save_id));
            return SIGIL_OK;
        }
    }
    for (size_t i = 0; i + 10 <= len; i++) {
        if (stem[i] == '(' && match_hex_run(stem, len, i + 1, 8, id8) && stem[i + 9] == ')') {
            memcpy(out->title_id, id8, 9);
            memcpy(out->raw_serial, id8, 9);
            sigil_lower_copy(out->title_id, out->save_id, sizeof(out->save_id));
            return SIGIL_OK;
        }
    }
    return SIGIL_ERR_NOT_FOUND;
}

static int try_gameid_bracket(const char *stem, size_t len,
                               sigil_platform platform, sigil_result *out) {
    out->usage = (platform == SIGIL_PLATFORM_WII)
        ? SIGIL_USAGE_FOLDER_EXACT
        : SIGIL_USAGE_FILE_PREFIX;
    for (size_t i = 0; i + 6 <= len; i++) {
        if (stem[i] != '[') continue;
        if (!sigil_is_upper(stem[i+1]) || !sigil_is_upper(stem[i+2])) continue;
        if (!(sigil_is_upper(stem[i+3]) || sigil_is_dig(stem[i+3]))) continue;
        if (!(sigil_is_upper(stem[i+4]) || sigil_is_dig(stem[i+4]))) continue;
        if (stem[i+5] != ']') continue;

        memcpy(out->raw_serial, stem + i + 1, 4);
        out->raw_serial[4] = '\0';
        sigil_hex_encode_4((const uint8_t *)(stem + i + 1), out->title_id);
        if (platform == SIGIL_PLATFORM_WII) {
            sigil_lower_copy(out->title_id, out->save_id, sizeof(out->save_id));
        } else {
            memcpy(out->save_id, out->raw_serial, strlen(out->raw_serial) + 1);
        }
        return SIGIL_OK;
    }
    return SIGIL_ERR_NOT_FOUND;
}

int sigil_filename_fallback(const char *filename_hint,
                            sigil_platform platform,
                            sigil_result *out) {
    if (!filename_hint || !out) return SIGIL_ERR_INVALID_ARG;

    char stem[256];
    stem_of(filename_hint, stem);
    size_t len = strlen(stem);
    if (len == 0) return SIGIL_ERR_NOT_FOUND;

    sigil_result_init(out);
    out->platform = platform;
    out->source = SIGIL_SOURCE_FILENAME;

    int rc = SIGIL_ERR_NOT_FOUND;
    switch (platform) {
    case SIGIL_PLATFORM_PSP:
        out->usage = SIGIL_USAGE_FOLDER_PREFIX;
        rc = try_psp(stem, len, out);
        break;
    case SIGIL_PLATFORM_PSVITA:
        out->usage = SIGIL_USAGE_FOLDER_EXACT;
        rc = try_psp(stem, len, out);
        break;
    case SIGIL_PLATFORM_PS3:
        /* A PS3 TITLE_ID has the PSP id's shape (BLUS31426), and its saves
         * are the folders starting with it, as ps3.c reports. */
        out->usage = SIGIL_USAGE_FOLDER_PREFIX;
        rc = try_psp(stem, len, out);
        break;
    case SIGIL_PLATFORM_PSX:
        out->usage = SIGIL_USAGE_FILE_PREFIX;
        rc = try_dashed_serial(stem, len, out);
        break;
    case SIGIL_PLATFORM_PS2:
        out->usage = SIGIL_USAGE_FOLDER_PREFIX;
        rc = try_dashed_serial(stem, len, out);
        if (rc == SIGIL_OK) sigil_ps2_save_id_stem(out->title_id, out->save_id);
        break;
    case SIGIL_PLATFORM_SWITCH:
        out->usage = SIGIL_USAGE_FOLDER_EXACT;
        rc = try_hex16(stem, len, out, true);
        if (rc == SIGIL_OK) sigil_switch_application_of_id(out->raw_serial, out->title_id);
        break;
    case SIGIL_PLATFORM_3DS:
        out->usage = SIGIL_USAGE_FOLDER_SPLIT;
        rc = try_3ds(stem, len, out);
        break;
    case SIGIL_PLATFORM_WIIU:
        out->usage = SIGIL_USAGE_FOLDER_EXACT;
        rc = try_wiiu(stem, len, out);
        break;
    case SIGIL_PLATFORM_WII:
    case SIGIL_PLATFORM_GAMECUBE:
        rc = try_gameid_bracket(stem, len, platform, out);
        break;
    case SIGIL_PLATFORM_AUTO:
        /* Nothing named a platform, so each pattern is tried in turn and the
         * one that matches names the console. */
        out->platform = SIGIL_PLATFORM_SWITCH;
        out->usage = SIGIL_USAGE_FOLDER_EXACT;
        if ((rc = try_hex16(stem, len, out, true)) == SIGIL_OK) {
            sigil_switch_application_of_id(out->raw_serial, out->title_id);
            break;
        }

        out->platform = SIGIL_PLATFORM_WIIU;
        if ((rc = try_wiiu(stem, len, out)) == SIGIL_OK) break;

        out->platform = SIGIL_PLATFORM_PS2;
        out->usage = SIGIL_USAGE_FOLDER_PREFIX;
        if ((rc = try_dashed_serial(stem, len, out)) == SIGIL_OK) {
            sigil_ps2_save_id_stem(out->title_id, out->save_id);
            break;
        }

        out->platform = SIGIL_PLATFORM_PSP;
        out->usage = SIGIL_USAGE_FOLDER_PREFIX;
        rc = try_psp(stem, len, out);
        if (rc == SIGIL_OK && is_vita_retail_id(out->title_id)) {
            out->platform = SIGIL_PLATFORM_PSVITA;
            out->usage = SIGIL_USAGE_FOLDER_EXACT;
        }
        break;
    default:
        /* A named platform with no naming pattern of its own: Dreamcast,
         * whose filenames carry no bracketed product number, Xbox, Xbox 360
         * and the cartridge consoles. The patterns above would hand back
         * another console's serial under it (a PS2 or PSP serial for a
         * Dreamcast disc). No pattern is better than a wrong one, and the
         * named platform stands. */
        rc = SIGIL_ERR_NOT_FOUND;
        break;
    }

    return rc;
}
