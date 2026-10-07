// SPDX-License-Identifier: MPL-2.0
/* The layout catalog reports what the rows hold: each layout's options with
 * their values and defaults, the region option, and whether restore needs a
 * profile or one of the game's files. */
#include "sigil.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int g_fails = 0;

static void fail(const char *where, const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", where, what);
    g_fails++;
}

static const sigil_layout_info *find(const sigil_layout_info *l, size_t n, const char *id) {
    for (size_t i = 0; i < n; i++) {
        if (strcmp(l[i].id, id) == 0) return &l[i];
    }
    return NULL;
}

static const sigil_layout_option *option(const sigil_layout_info *info, const char *key) {
    for (size_t i = 0; info && i < info->option_count; i++) {
        if (strcmp(info->options[i].key, key) == 0) return &info->options[i];
    }
    return NULL;
}

static bool has_value(const sigil_layout_option *o, const char *value) {
    for (size_t i = 0; o && i < o->value_count; i++) {
        if (strcmp(o->values[i], value) == 0) return true;
    }
    return false;
}

static void check_options(void) {
    sigil_layout_info *l = NULL;
    size_t n = 0;
    if (sigil_layouts("psx", &l, &n) != SIGIL_OK || n == 0) { fail("psx", "no layouts"); return; }
    if (strcmp(l[0].id, "libretro") != 0) fail("psx", "the libretro default row isn't first");
    if (find(l, n, "cemu")) fail("psx", "another platform's row is listed");
    const sigil_layout_option *card = option(find(l, n, "duckstation"), "Card1Type");
    if (!card || card->value_count != 4 || !has_value(card, "PerGame") || !has_value(card, "Shared") ||
        strcmp(card->default_value, "PerGameTitle") != 0) {
        fail("duckstation", "Card1Type's values and default");
    }
    const sigil_layout_option *slot = option(find(l, n, "pcsx_rearmed"), "pcsx_rearmed_memcard2");
    if (!slot || strcmp(slot->default_value, "shared") != 0) fail("pcsx_rearmed", "memcard2 defaults to shared");
    if (!option(find(l, n, "mednafen_psx"), "beetle_psx_memcard_left_index") ||
        !option(find(l, n, "mednafen_psx_hw"), "beetle_psx_hw_memcard_left_index")) {
        fail("beetle psx", "card index options under each build's prefix");
    }
    sigil_layouts_free(l);

    if (sigil_layouts("ps2", &l, &n) != SIGIL_OK) return;
    const sigil_layout_option *name = option(find(l, n, "pcsx2_standalone"), "Slot1_Filename");
    if (!name || name->value_count != 0 || strcmp(name->default_value, "Mcd001.ps2") != 0) {
        fail("pcsx2_standalone", "Slot1_Filename is free-form, Mcd001.ps2 by default");
    }
    sigil_layouts_free(l);

    if (sigil_layouts("segacd", &l, &n) != SIGIL_OK) return;
    const sigil_layout_info *gpgx = find(l, n, "genesis_plus_gx");
    const sigil_layout_option *region = option(gpgx, "genesis_plus_gx_region_detect");
    const sigil_layout_option *cart = option(gpgx, "genesis_plus_gx_cart_size");
    const sigil_layout_option *bram = option(gpgx, "genesis_plus_gx_system_bram");
    if (!gpgx || strcmp(gpgx->region_option, "genesis_plus_gx_region_detect") != 0 || !region ||
        strcmp(region->default_value, "auto") != 0 || !has_value(region, "pal")) {
        fail("genesis_plus_gx", "the region option, its values and default");
    }
    if (!cart || cart->value_count != 6 || strcmp(cart->default_value, "4meg") != 0) fail("genesis_plus_gx", "cart sizes");
    if (!bram || strcmp(bram->default_value, "per bios") != 0 || !has_value(bram, "per game")) {
        fail("genesis_plus_gx", "system_bram shared by default");
    }
    sigil_layouts_free(l);
}

static void check_flags(void) {
    sigil_layout_info *l = NULL;
    size_t n = 0;
    if (sigil_layouts(NULL, &l, &n) != SIGIL_OK) { fail("all", "no layouts"); return; }
    static const struct { const char *id; const char *platform; int profiles, needs_existing; } WANT[] = {
        { "mupen64plus_next", "n64", 0, 0 }, { "mupen64plus_standalone", "n64", 0, 1 }, { "m64plus_fz", "n64", 0, 1 },
        { "project64", "n64", 0, 1 }, { "ryujinx", "switch", 1, 1 }, { "eden", "switch", 1, 0 },
        { "skyline", "switch", 0, 0 }, { "cemu", "wiiu", 1, 0 }, { "ppsspp", "psp", 0, 0 }, { "azahar", "3ds", 0, 0 },
        { "duckstation", "psx", 0, 0 },
    };
    for (size_t i = 0; i < sizeof(WANT) / sizeof(WANT[0]); i++) {
        const sigil_layout_info *info = find(l, n, WANT[i].id);
        if (!info || strcmp(info->platform, WANT[i].platform) != 0 || info->profiles != WANT[i].profiles ||
            info->needs_existing != WANT[i].needs_existing) {
            fail(WANT[i].id, "platform, profiles or needs_existing");
        }
    }
    size_t dolphins = 0;
    for (size_t i = 0; i < n; i++) dolphins += strcmp(l[i].id, "dolphin") == 0;
    if (dolphins != 2) fail("dolphin", "one row per platform (GameCube, Wii)");
    sigil_layouts_free(l);
}

int main(void) {
    check_options();
    check_flags();
    printf("layouts: %d failures\n", g_fails);
    return g_fails ? 1 : 0;
}
