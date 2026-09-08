// SPDX-License-Identifier: MPL-2.0
#include "save_layout.h"

/* Every row below was read from the core's own source or its libretro docs
 * page; the file names are the core's literals, not analogies. A core absent
 * from the table persists through RETRO_MEMORY_SAVE_RAM alone and takes the
 * libretro default row. */

#define M(t, r)                 { t, SIGIL_SAVE_ROLE_##r, NULL, NULL, false }
#define M_OPT(t, r, k, v, d)    { t, SIGIL_SAVE_ROLE_##r, k, v, d }
#define S(t)                    { t, NULL, NULL, false }
#define S_OPT(t, k, v, d)       { t, k, v, d }
#define COUNT(a)                (sizeof(a) / sizeof((a)[0]))

/* RetroArch persists SAVE_RAM as <stem>.srm and RETRO_MEMORY_RTC as
 * <stem>.rtc (save.c, path_init_savefile_rtc). */
static const sigil_layout_member LIBRETRO_DEFAULT_MEMBERS[] = {
    M("{stem}.srm", PRIMARY),
    M("{stem}.rtc", RTC),
};

/* Cores with no RTC region at all: vba_next, gpsp (libretro.c memory maps). */
static const sigil_layout_member SRM_ONLY_MEMBERS[] = {
    M("{stem}.srm", PRIMARY),
};

/* Genesis Plus GX Sega CD, libretro/libretro.c check_variables and bram_save:
 * per-game names only under "per game"; the per-bios and per-cart defaults
 * write one file for every game. */
static const sigil_layout_member GPGX_SEGACD_MEMBERS[] = {
    M("{stem}.srm", PRIMARY),
    M_OPT("{stem}.brm", SIDECAR, "genesis_plus_gx_system_bram", "per game", false),
    M_OPT("{stem}_{cart_size}_cart.brm", SIDECAR, "genesis_plus_gx_cart_bram", "per game", false),
};
static const sigil_layout_shared GPGX_SEGACD_SHARED[] = {
    S_OPT("scd_E.brm", "genesis_plus_gx_system_bram", "per bios", true),
    S_OPT("scd_U.brm", "genesis_plus_gx_system_bram", "per bios", true),
    S_OPT("scd_J.brm", "genesis_plus_gx_system_bram", "per bios", true),
    S_OPT("{cart_size}_cart.brm", "genesis_plus_gx_cart_bram", "per cart", true),
};

/* Beetle PSX HW: card 0 is SAVE_RAM under the libretro method or
 * <content>.<index>.mcr under the mednafen method; every other card is a
 * file (commit 707d1be). Shared names from docs.libretro.com/library/beetle_psx_hw. */
static const sigil_layout_member BEETLE_PSX_MEMBERS[] = {
    M_OPT("{stem}.srm", PRIMARY, "beetle_psx_hw_use_mednafen_memcard0_method", "libretro", true),
    M_OPT("{stem}.{left_index}.mcr", PRIMARY, "beetle_psx_hw_use_mednafen_memcard0_method", "mednafen", false),
    M_OPT("{stem}.{right_index}.mcr", SIDECAR, "beetle_psx_hw_enable_memcard1", "enabled", false),
};
static const sigil_layout_shared BEETLE_PSX_SHARED[] = {
    S_OPT("mednafen_psx_libretro_shared.0.mcr", "beetle_psx_hw_shared_memory_cards", "enabled", false),
    S_OPT("mednafen_psx_libretro_shared.1.mcr", "beetle_psx_hw_shared_memory_cards", "enabled", false),
};

/* Beetle Saturn, mednafen/ss/ss.c: internal backup RAM is SAVE_RAM under the
 * libretro method or <content>.bkr under mednafen; the cart (.bcr) and the
 * SMPC clock (.smpc) are always files. */
static const sigil_layout_member BEETLE_SATURN_MEMBERS[] = {
    M_OPT("{stem}.srm", PRIMARY, "beetle_saturn_save_method", "libretro", true),
    M_OPT("{stem}.bkr", PRIMARY, "beetle_saturn_save_method", "mednafen", false),
    M("{stem}.bcr", SIDECAR),
    M("{stem}.smpc", SIDECAR),
};
static const sigil_layout_shared BEETLE_SATURN_SHARED[] = {
    S_OPT("mednafen_saturn_libretro_shared.bkr", "beetle_saturn_shared_int", "enabled", false),
    S_OPT("mednafen_saturn_libretro_shared.smpc", "beetle_saturn_shared_int", "enabled", false),
    S_OPT("mednafen_saturn_libretro_shared.bcr", "beetle_saturn_shared_ext", "enabled", false),
};

/* Beetle NeoPop, mednafen/ngp/system.c system_io_flash_write. */
static const sigil_layout_member BEETLE_NGP_MEMBERS[] = {
    M("{stem}.flash", PRIMARY),
};

/* Opera, opera_lr_nvram.c: per-game by default, shared on request. */
static const sigil_layout_member OPERA_MEMBERS[] = {
    M_OPT("opera/per_game/{stem}.{nvram_version}.srm", PRIMARY, "opera_nvram_storage", "per game", true),
};
static const sigil_layout_shared OPERA_SHARED[] = {
    S_OPT("opera/shared/nvram.{nvram_version}.srm", "opera_nvram_storage", "shared", false),
};
static const char *const OPERA_SUBDIRS[] = { "opera/per_game", "opera/shared" };

/* PokeMini libretro.c: <basename>.eep written at unload. */
static const sigil_layout_member POKEMINI_MEMBERS[] = {
    M("{stem}.eep", PRIMARY),
};

/* Handy libretro.cpp: <content>.eeprom written at retro_deinit. */
static const sigil_layout_member HANDY_MEMBERS[] = {
    M("{stem}.eeprom", PRIMARY),
};

/* Legacy melonDS libretro.cpp: <game>.sav through its own flush timer. */
static const sigil_layout_member MELONDS_MEMBERS[] = {
    M("{stem}.sav", PRIMARY),
};

/* FBNeo retro_common.cpp / eeprom.cpp: everything under <save>/fbneo/.
 * Memcard mode values are the English table of a localized option. */
static const sigil_layout_member FBNEO_MEMBERS[] = {
    M("fbneo/{romset}.fs", PRIMARY),
    M("fbneo/{romset}.nv", SIDECAR),
    M_OPT("fbneo/{romset}.memcard", SIDECAR, "fbneo-memcard-mode", "per-game", false),
};
static const sigil_layout_shared FBNEO_SHARED[] = {
    S_OPT("fbneo/shared.memcard", "fbneo-memcard-mode", "shared", false),
};
static const char *const FBNEO_SUBDIRS[] = { "fbneo" };

/* MAME 2003 Plus fileio.c: nvram/ and hi/ under the APPNAME subfolder while
 * core_save_subfolder is enabled (its default), else directly under the root. */
static const sigil_layout_member MAME2003_PLUS_MEMBERS[] = {
    M_OPT("mame2003-plus/nvram/{romset}.nv", PRIMARY, "mame2003-plus_core_save_subfolder", "enabled", true),
    M_OPT("mame2003-plus/hi/{romset}.hi", SIDECAR, "mame2003-plus_core_save_subfolder", "enabled", true),
    M_OPT("nvram/{romset}.nv", PRIMARY, "mame2003-plus_core_save_subfolder", "disabled", false),
    M_OPT("hi/{romset}.hi", SIDECAR, "mame2003-plus_core_save_subfolder", "disabled", false),
};
static const char *const MAME2003_PLUS_SUBDIRS[] = {
    "mame2003-plus/nvram", "mame2003-plus/hi", "nvram", "hi"
};

/* DOSBox Pure DBP_GetSaveFile: one zip per content; it travels as a file. */
static const sigil_layout_member DOSBOX_PURE_MEMBERS[] = {
    M("{stem}.pure.zip", PRIMARY),
};

/* SAME CDI retro_init.cpp: MAME's nvram directory under a per-game folder
 * while same_cdi_nvram_saves is enabled (its default). */
static const sigil_layout_member SAME_CDI_MEMBERS[] = {
    M_OPT("same_cdi/nvram/{stem}/", PRIMARY, "same_cdi_nvram_saves", "enabled", true),
};
static const char *const SAME_CDI_SUBDIRS[] = { "same_cdi/nvram" };

/* Nestopia libretro.cpp SAVE_FDS: the disk image or a patch beside the SRAM. */
static const sigil_layout_member NESTOPIA_FDS_MEMBERS[] = {
    M("{stem}.srm", PRIMARY),
    M_OPT("{stem}.sav", SIDECAR, "nestopia_fds_savefile_format", "sav_ups", true),
    M_OPT("{stem}.ups", SIDECAR, "nestopia_fds_savefile_format", "ups", false),
    M_OPT("{stem}.ips", SIDECAR, "nestopia_fds_savefile_format", "ips", false),
};

/* bsnes program.cpp: its own .srm for every cart, .rtc only for Game Boy
 * carts; SNES clock chips persist nothing there. */
static const sigil_layout_member BSNES_SNES_MEMBERS[] = {
    M("{stem}.srm", PRIMARY),
};

#define ROW(l, p, m)            { l, p, m, COUNT(m), NULL, 0, NULL, 0 }
#define ROW_SHARED(l, p, m, s)  { l, p, m, COUNT(m), s, COUNT(s), NULL, 0 }
#define ROW_FULL(l, p, m, s, d) { l, p, m, COUNT(m), s, COUNT(s), d, COUNT(d) }
#define ROW_DIRS(l, p, m, d)    { l, p, m, COUNT(m), NULL, 0, d, COUNT(d) }

static const sigil_layout LIBRETRO_DEFAULT = ROW("libretro", NULL, LIBRETRO_DEFAULT_MEMBERS);

static const sigil_layout LAYOUTS[] = {
    ROW("vba_next", NULL, SRM_ONLY_MEMBERS),
    ROW("gpsp", NULL, SRM_ONLY_MEMBERS),
    ROW("bsnes", "snes", BSNES_SNES_MEMBERS),
    ROW_SHARED("genesis_plus_gx", "segacd", GPGX_SEGACD_MEMBERS, GPGX_SEGACD_SHARED),
    ROW_SHARED("mednafen_psx_hw", NULL, BEETLE_PSX_MEMBERS, BEETLE_PSX_SHARED),
    ROW_SHARED("mednafen_saturn", NULL, BEETLE_SATURN_MEMBERS, BEETLE_SATURN_SHARED),
    ROW("mednafen_ngp", NULL, BEETLE_NGP_MEMBERS),
    ROW_FULL("opera", NULL, OPERA_MEMBERS, OPERA_SHARED, OPERA_SUBDIRS),
    ROW("pokemini", NULL, POKEMINI_MEMBERS),
    ROW("handy", NULL, HANDY_MEMBERS),
    ROW("melonds", NULL, MELONDS_MEMBERS),
    ROW_FULL("fbneo", NULL, FBNEO_MEMBERS, FBNEO_SHARED, FBNEO_SUBDIRS),
    ROW_DIRS("mame2003_plus", NULL, MAME2003_PLUS_MEMBERS, MAME2003_PLUS_SUBDIRS),
    ROW("dosbox_pure", NULL, DOSBOX_PURE_MEMBERS),
    ROW_DIRS("same_cdi", NULL, SAME_CDI_MEMBERS, SAME_CDI_SUBDIRS),
    ROW("nestopia", "fds", NESTOPIA_FDS_MEMBERS),
};

/* Rows name platforms by the slugs in the README; consumers may pass their
 * own short forms, which resolve here the way sigil_platform_from_slug
 * resolves title-id platforms. */
static const char *canonical_platform(const char *slug) {
    if (!slug) return NULL;
    if (strcmp(slug, "scd") == 0 || strcmp(slug, "sega_cd") == 0 || strcmp(slug, "sega-cd") == 0
        || strcmp(slug, "mega_cd") == 0 || strcmp(slug, "mega-cd") == 0 || strcmp(slug, "megacd") == 0) {
        return "segacd";
    }
    if (strcmp(slug, "sfc") == 0 || strcmp(slug, "sfam") == 0) return "snes";
    if (strcmp(slug, "ps1") == 0 || strcmp(slug, "playstation") == 0) return "psx";
    if (strcmp(slug, "famicom_disk_system") == 0 || strcmp(slug, "fds") == 0) return "fds";
    return slug;
}

static bool platform_matches(const char *row_platform, const char *platform) {
    if (!row_platform) return true;
    if (!platform) return false;
    return strcmp(row_platform, canonical_platform(platform)) == 0;
}

const sigil_layout *sigil_layout_find(const char *layout, const char *platform) {
    if (!layout) return &LIBRETRO_DEFAULT;
    const sigil_layout *any_platform = NULL;
    for (size_t i = 0; i < COUNT(LAYOUTS); i++) {
        if (strcmp(LAYOUTS[i].layout, layout) != 0) continue;
        if (LAYOUTS[i].platform && platform_matches(LAYOUTS[i].platform, platform)) return &LAYOUTS[i];
        if (!LAYOUTS[i].platform) any_platform = &LAYOUTS[i];
    }
    return any_platform ? any_platform : &LIBRETRO_DEFAULT;
}

size_t sigil_layout_rows(const char *layout, const sigil_layout **out, size_t cap) {
    size_t n = 0;
    if (!layout) return 0;
    for (size_t i = 0; i < COUNT(LAYOUTS) && n < cap; i++) {
        if (strcmp(LAYOUTS[i].layout, layout) == 0) out[n++] = &LAYOUTS[i];
    }
    return n;
}

size_t sigil_save_layout_subdirs(const char *layout, const char **out, size_t cap) {
    const sigil_layout *rows[8];
    size_t row_count = sigil_layout_rows(layout, rows, COUNT(rows));
    size_t n = 0;
    for (size_t r = 0; r < row_count; r++) {
        for (size_t i = 0; i < rows[r]->subdir_count && n < cap; i++) {
            out[n++] = rows[r]->subdirs[i];
        }
    }
    return n;
}
