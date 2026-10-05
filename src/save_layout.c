// SPDX-License-Identifier: MPL-2.0
#include "save_layout.h"
#include "card_saturn.h"

#define M(t, r)                 { t, SIGIL_SAVE_ROLE_##r, NULL, NULL, false, SIGIL_DEVICE_NONE }
#define M_OPT(t, r, k, v, d)    { t, SIGIL_SAVE_ROLE_##r, k, v, d, SIGIL_DEVICE_NONE }
#define M_DEV(t, r, dev)        { t, SIGIL_SAVE_ROLE_##r, NULL, NULL, false, SIGIL_DEVICE_##dev }
#define M_OPT_DEV(t, r, k, v, d, dev) { t, SIGIL_SAVE_ROLE_##r, k, v, d, SIGIL_DEVICE_##dev }
#define S(t)                    { t, NULL, NULL, false, SIGIL_DEVICE_NONE, 0 }
#define S_OPT(t, k, v, d)       { t, k, v, d, SIGIL_DEVICE_NONE, 0 }
#define S_OPT_DEV(t, k, v, d, dev, region) { t, k, v, d, SIGIL_DEVICE_##dev, region }
#define M_OPT2_DEV(t, r, k, v, d, k2, v2, d2, dev) { t, SIGIL_SAVE_ROLE_##r, k, v, d, SIGIL_DEVICE_##dev, k2, v2, d2 }
#define COUNT(a)                (sizeof(a) / sizeof((a)[0]))

static const sigil_layout_member LIBRETRO_DEFAULT_MEMBERS[] = {
    M("{stem}.srm", PRIMARY),
    M("{stem}.rtc", RTC),
};

static const sigil_layout_member SRM_ONLY_MEMBERS[] = {
    M("{stem}.srm", PRIMARY),
};

static const sigil_layout_member GPGX_SEGACD_MEMBERS[] = {
    M_OPT_DEV("{stem}.brm", PRIMARY, "genesis_plus_gx_system_bram", "per game", false, INTERNAL),
    M_OPT_DEV("{stem}_{cart_size}_cart.brm", SIDECAR, "genesis_plus_gx_cart_bram", "per game", false, CART),
};
static const sigil_layout_shared GPGX_SEGACD_SHARED[] = {
    S_OPT_DEV("scd_E.brm", "genesis_plus_gx_system_bram", "per bios", true, INTERNAL, 'E'),
    S_OPT_DEV("scd_U.brm", "genesis_plus_gx_system_bram", "per bios", true, INTERNAL, 'U'),
    S_OPT_DEV("scd_J.brm", "genesis_plus_gx_system_bram", "per bios", true, INTERNAL, 'J'),
    S_OPT_DEV("{cart_size}_cart.brm", "genesis_plus_gx_cart_bram", "per cart", true, CART, 0),
};

/* Beetle PSX (libretro.c MDFN_MakeFName): shared cards swap the game's name
 * for mednafen_psx_libretro_shared on every .mcr card and keep its index. Slot
 * 1 under the libretro method is the .srm, which is never shared; slot 2 exists
 * only with enable_memcard1. The hardware build reads beetle_psx_hw_* keys and
 * the software build beetle_psx_*. */
#define BEETLE_PSX_ROW(NAME, P)                                                                              \
    static const sigil_layout_member NAME##_MEMBERS[] = {                                                    \
        M_OPT("{stem}.srm", PRIMARY, P "use_mednafen_memcard0_method", "libretro", true),                     \
        { .template_ = "{stem}.{left_index}.mcr", .role = SIGIL_SAVE_ROLE_PRIMARY,                           \
          .opt_key = P "use_mednafen_memcard0_method", .opt_value = "mednafen", .opt_default = false,         \
          .opt2_key = P "shared_memory_cards", .opt2_value = "disabled", .opt2_default = true },              \
        { .template_ = "{stem}.{right_index}.mcr", .role = SIGIL_SAVE_ROLE_SIDECAR,                          \
          .opt_key = P "enable_memcard1", .opt_value = "enabled", .opt_default = false,                       \
          .opt2_key = P "shared_memory_cards", .opt2_value = "disabled", .opt2_default = true },              \
    };                                                                                                       \
    static const sigil_layout_shared NAME##_SHARED[] = {                                                     \
        { .template_ = "mednafen_psx_libretro_shared.{left_index}.mcr",                                      \
          .opt_key = P "shared_memory_cards", .opt_value = "enabled", .opt_default = false,                   \
          .opt2_key = P "use_mednafen_memcard0_method", .opt2_value = "mednafen", .opt2_default = false },    \
        { .template_ = "mednafen_psx_libretro_shared.{right_index}.mcr",                                     \
          .opt_key = P "shared_memory_cards", .opt_value = "enabled", .opt_default = false,                   \
          .opt2_key = P "enable_memcard1", .opt2_value = "enabled", .opt2_default = false },                  \
    }
BEETLE_PSX_ROW(BEETLE_PSX_HW, "beetle_psx_hw_");
BEETLE_PSX_ROW(BEETLE_PSX, "beetle_psx_");

/* `.srm` is always per game. The `.bkr` that save_method=mednafen uses, the
 * `.smpc` and the cart `.bcr` move to the shared files with shared_int and
 * shared_ext (sega.md section 1, Beetle Saturn rows). The `.bcr` is always a
 * 4 Mbit cart, as mednafen's uncompressed samples are, and Kronos fixes the
 * same cart for its Beetle-compatible saves. */
static const sigil_layout_member BEETLE_SATURN_MEMBERS[] = {
    { .template_ = "{stem}.srm", .role = SIGIL_SAVE_ROLE_PRIMARY, .opt_key = "beetle_saturn_save_method",
      .opt_value = "libretro", .opt_default = true, .device = SIGIL_DEVICE_INTERNAL, .new_size = SATURN_INTERNAL_SIZE },
    { .template_ = "{stem}.bkr", .role = SIGIL_SAVE_ROLE_PRIMARY, .opt_key = "beetle_saturn_save_method",
      .opt_value = "mednafen", .opt_default = false, .device = SIGIL_DEVICE_INTERNAL, .opt2_key = "beetle_saturn_shared_int",
      .opt2_value = "disabled", .opt2_default = true, .new_size = SATURN_INTERNAL_SIZE },
    { .template_ = "{stem}.bcr", .role = SIGIL_SAVE_ROLE_SIDECAR, .opt_key = "beetle_saturn_shared_ext",
      .opt_value = "disabled", .opt_default = true, .device = SIGIL_DEVICE_CART, .new_size = SATURN_CART_SIZE },
    M_OPT("{stem}.smpc", SIDECAR, "beetle_saturn_shared_int", "disabled", true),
};
static const sigil_layout_shared BEETLE_SATURN_SHARED[] = {
    { .template_ = "mednafen_saturn_libretro_shared.bkr", .opt_key = "beetle_saturn_shared_int", .opt_value = "enabled",
      .opt_default = false, .device = SIGIL_DEVICE_INTERNAL, .opt2_key = "beetle_saturn_save_method",
      .opt2_value = "mednafen", .opt2_default = false, .new_size = SATURN_INTERNAL_SIZE },
    S_OPT("mednafen_saturn_libretro_shared.smpc", "beetle_saturn_shared_int", "enabled", false),
    { .template_ = "mednafen_saturn_libretro_shared.bcr", .opt_key = "beetle_saturn_shared_ext", .opt_value = "enabled",
      .opt_default = false, .device = SIGIL_DEVICE_CART, .new_size = SATURN_CART_SIZE },
};

/* Kronos (libretro/yabause `kronos`): libretro.c configure_saturn_addon_cart
 * and the bup_path in retro_load_game; the cart file is named by
 * kronos_addon_cartridge, default 512K_backup_ram. */
#define KRONOS_INTERNAL(t) \
    { .template_ = t, .role = SIGIL_SAVE_ROLE_PRIMARY, .opt_key = "kronos_use_beetle_saves", .opt_value = "disabled", \
      .opt_default = true, .device = SIGIL_DEVICE_INTERNAL, .new_size = SATURN_INTERNAL_SIZE }
#define KRONOS_CART(t, value, d, size) \
    { .template_ = t, .role = SIGIL_SAVE_ROLE_SIDECAR, .opt_key = "kronos_use_beetle_saves", .opt_value = "disabled", \
      .opt_default = true, .device = SIGIL_DEVICE_CART, .opt2_key = "kronos_addon_cartridge", .opt2_value = value, \
      .opt2_default = d, .new_size = size }
static const sigil_layout_member KRONOS_MEMBERS[] = {
    KRONOS_INTERNAL("kronos/saturn/{stem}.ram"),
    { .template_ = "{stem}.bkr", .role = SIGIL_SAVE_ROLE_PRIMARY, .opt_key = "kronos_use_beetle_saves",
      .opt_value = "enabled", .opt_default = false, .device = SIGIL_DEVICE_INTERNAL, .new_size = SATURN_INTERNAL_SIZE },
    KRONOS_CART("kronos/saturn/{stem}-ext512K.ram", "512K_backup_ram", true, 512u * 1024u),
    KRONOS_CART("kronos/saturn/{stem}-ext1M.ram", "1M_backup_ram", false, 1024u * 1024u),
    KRONOS_CART("kronos/saturn/{stem}-ext2M.ram", "2M_backup_ram", false, 2048u * 1024u),
    KRONOS_CART("kronos/saturn/{stem}-ext4M.ram", "4M_backup_ram", false, 4096u * 1024u),
    { .template_ = "{stem}.bcr", .role = SIGIL_SAVE_ROLE_SIDECAR, .opt_key = "kronos_use_beetle_saves",
      .opt_value = "enabled", .opt_default = false, .device = SIGIL_DEVICE_CART, .new_size = SATURN_CART_SIZE },
};
static const char *const KRONOS_SUBDIRS[] = { "kronos/saturn" };

/* yabause (libretro master) writes `{stem}.srm` itself, 64 KiB expanded with
 * 0xFF filler (sega.md section 1). */
static const sigil_layout_member YABAUSE_MEMBERS[] = {
    { .template_ = "{stem}.srm", .role = SIGIL_SAVE_ROLE_PRIMARY, .device = SIGIL_DEVICE_INTERNAL,
      .form = SIGIL_FORM_EXPANDED_FF, .new_size = SATURN_INTERNAL_SIZE },
};

/* Yaba Sanshiro keeps one memory-mapped backup.bin for every game, 8 MiB
 * expanded with 0xFF filler. */
static const sigil_layout_shared YABASANSHIRO_SHARED[] = {
    { .template_ = "yabasanshiro/backup.bin", .device = SIGIL_DEVICE_INTERNAL, .form = SIGIL_FORM_EXPANDED_FF,
      .new_size = 4u * 1024u * 1024u },
};
static const char *const YABASANSHIRO_SUBDIRS[] = { "yabasanshiro" };

/* flycast libretro (flyinghead/flycast shell/libretro/oslib.cpp getVmuPath).
 * reicast_per_content_vmus "VMU A1" keeps A1 per game in the save folder and
 * the other ports shared in the system folder, outside the save root, so
 * only A1 syncs in that mode; "All VMUs" keeps every port per game;
 * "disabled" (default) shares every port as vmu_save_{port}.bin in the
 * system folder's dc/, which a client passes as the save root. A per-game
 * file is {gameId}.{port}.bin, or the legacy {stem}.{port}.bin flycast
 * still reads. */
#define FLY_OPT "reicast_per_content_vmus"
#define FLY_ALL(port, dev) \
    M_OPT_DEV("{dc_vmu_id}." port ".bin", SIDECAR, FLY_OPT, "All VMUs", false, dev), \
    M_OPT_DEV("{stem}." port ".bin", SIDECAR, FLY_OPT, "All VMUs", false, dev)
#define FLY_SHARED(port, dev) S_OPT_DEV("vmu_save_" port ".bin", FLY_OPT, "disabled", true, dev, 0)
static const sigil_layout_member FLYCAST_MEMBERS[] = {
    M_OPT_DEV("{dc_vmu_id}.A1.bin", PRIMARY, FLY_OPT, "VMU A1", false, VMU_A1),
    M_OPT_DEV("{dc_vmu_id}.A1.bin", PRIMARY, FLY_OPT, "All VMUs", false, VMU_A1),
    M_OPT_DEV("{stem}.A1.bin", PRIMARY, FLY_OPT, "VMU A1", false, VMU_A1),
    M_OPT_DEV("{stem}.A1.bin", PRIMARY, FLY_OPT, "All VMUs", false, VMU_A1),
    FLY_ALL("A2", VMU_A2), FLY_ALL("B1", VMU_B1), FLY_ALL("B2", VMU_B2), FLY_ALL("C1", VMU_C1),
    FLY_ALL("C2", VMU_C2), FLY_ALL("D1", VMU_D1), FLY_ALL("D2", VMU_D2),
};
static const sigil_layout_shared FLYCAST_SHARED[] = {
    FLY_SHARED("A1", VMU_A1), FLY_SHARED("A2", VMU_A2), FLY_SHARED("B1", VMU_B1), FLY_SHARED("B2", VMU_B2),
    FLY_SHARED("C1", VMU_C1), FLY_SHARED("C2", VMU_C2), FLY_SHARED("D1", VMU_D1), FLY_SHARED("D2", VMU_D2),
};

/* Standalone flycast (core/oslib/oslib.cpp): PerGameVmu (default yes) keeps
 * A1 as {gameId}_vmu_save_A1.bin, or the legacy {stem}_vmu_save_A1.bin; the
 * other ports, and A1 with PerGameVmu off, are vmu_save_{port}.bin, all in
 * the VMU folder. */
static const sigil_layout_member FLYCAST_STANDALONE_MEMBERS[] = {
    M_OPT_DEV("{dc_vmu_id}_vmu_save_A1.bin", PRIMARY, "PerGameVmu", "yes", true, VMU_A1),
    M_OPT_DEV("{stem}_vmu_save_A1.bin", PRIMARY, "PerGameVmu", "yes", true, VMU_A1),
};
static const sigil_layout_shared FLYCAST_STANDALONE_SHARED[] = {
    S_OPT_DEV("vmu_save_A1.bin", "PerGameVmu", "no", false, VMU_A1, 0),
    S_OPT_DEV("vmu_save_A2.bin", NULL, NULL, false, VMU_A2, 0),
    S_OPT_DEV("vmu_save_B1.bin", NULL, NULL, false, VMU_B1, 0),
    S_OPT_DEV("vmu_save_B2.bin", NULL, NULL, false, VMU_B2, 0),
    S_OPT_DEV("vmu_save_C1.bin", NULL, NULL, false, VMU_C1, 0),
    S_OPT_DEV("vmu_save_C2.bin", NULL, NULL, false, VMU_C2, 0),
    S_OPT_DEV("vmu_save_D1.bin", NULL, NULL, false, VMU_D1, 0),
    S_OPT_DEV("vmu_save_D2.bin", NULL, NULL, false, VMU_D2, 0),
};

/* Dolphin (dolphin-emu Config/MainSettings.cpp GetGCIFolderPath,
 * GetMemcardPath): slot A is the GCI folder {User}/GC/{region}/Card A by
 * default (SlotA = 8 in Dolphin.ini), a raw card {User}/GC/MemoryCardA.{region}.raw
 * with SlotA = 1, the name carrying .{blocks} for cards under 2043 blocks.
 * The libretro core's User folder is the save folder's User/. */
#define GC_FOLDER(t) { .template_ = t, .opt_key = "SlotA", .opt_value = "8", .opt_default = true, \
                       .device = SIGIL_DEVICE_GC_FOLDER }
/* MemoryCardSize (-1 for 2043 blocks, 0 to 4 for 59 to 1019) picks the raw
 * card; without it every name applies and sigil_sync_gather_cards refuses
 * when more than one is there. */
#define GC_CARD(t, size) \
    { .template_ = t, .opt_key = "SlotA", .opt_value = "1", .opt_default = false, .device = SIGIL_DEVICE_GC_CARD, \
      .opt2_key = "MemoryCardSize", .opt2_value = size, .opt2_default = true }
#define GC_SLOT_A(user) \
    GC_FOLDER(user "GC/{gc_region}/Card A/"), \
    GC_CARD(user "GC/MemoryCardA.{gc_region}.raw", "-1"), GC_CARD(user "GC/MemoryCardA.{gc_region}.1019.raw", "4"), \
    GC_CARD(user "GC/MemoryCardA.{gc_region}.507.raw", "3"), GC_CARD(user "GC/MemoryCardA.{gc_region}.251.raw", "2"), \
    GC_CARD(user "GC/MemoryCardA.{gc_region}.123.raw", "1"), GC_CARD(user "GC/MemoryCardA.{gc_region}.59.raw", "0")
static const sigil_layout_shared DOLPHIN_SHARED[] = { GC_SLOT_A("User/") };
static const sigil_layout_shared DOLPHIN_STANDALONE_SHARED[] = { GC_SLOT_A("") };
static const char *const DOLPHIN_SUBDIRS[] = { "User/GC" };
static const char *const DOLPHIN_STANDALONE_SUBDIRS[] = { "GC" };

/* pcsx_rearmed (frontend/libretro.c load_memcards): slot 1 is the frontend's
 * .srm in libretro mode, slot 2 has no libretro mode, and either slot can be
 * a card named by the disc's id or one shared by every game. */
static const sigil_layout_member PCSX_REARMED_MEMBERS[] = {
    M_OPT("{stem}.srm", PRIMARY, "pcsx_rearmed_memcard1", "libretro", true),
    M_OPT("{pcsx_serial}_1.mcd", PRIMARY, "pcsx_rearmed_memcard1", "serial", false),
    M_OPT("{pcsx_serial}_2.mcd", SIDECAR, "pcsx_rearmed_memcard2", "serial", false),
};
static const sigil_layout_shared PCSX_REARMED_SHARED[] = {
    S_OPT("pcsx-card1.mcd", "pcsx_rearmed_memcard1", "shared", false),
    S_OPT("pcsx-card2.mcd", "pcsx_rearmed_memcard2", "shared", true),
};

/* SwanStation (libretro/swanstation libretro_host_interface.cpp, system.cpp):
 * slot 1 is the frontend's .srm in Libretro mode, the default; either slot can
 * be a card the core writes in the save folder, named by the disc's code
 * (PerGame) or the content's stem (PerGameTitle), or one every game shares. */
#define SWAN_OPT "swanstation_MemoryCards_"
static const sigil_layout_member SWANSTATION_MEMBERS[] = {
    M_OPT("{stem}.srm", PRIMARY, SWAN_OPT "Card1Type", "Libretro", true),
    M_OPT("{title_id}_1.mcd", PRIMARY, SWAN_OPT "Card1Type", "PerGame", false),
    M_OPT("{stem}_1.mcd", PRIMARY, SWAN_OPT "Card1Type", "PerGameTitle", false),
    M_OPT("{title_id}_2.mcd", SIDECAR, SWAN_OPT "Card2Type", "PerGame", false),
    M_OPT("{stem}_2.mcd", SIDECAR, SWAN_OPT "Card2Type", "PerGameTitle", false),
};
static const sigil_layout_shared SWANSTATION_SHARED[] = {
    S_OPT("duckstation_shared_card_1.mcd", SWAN_OPT "Card1Type", "Shared", false),
    S_OPT("duckstation_shared_card_2.mcd", SWAN_OPT "Card2Type", "Shared", false),
};

/* DuckStation (stenzek/duckstation settings.cpp, system.cpp), rooted at the
 * data folder holding memcards/: [MemoryCards] Card1Type defaults to
 * PerGameTitle, which names the card after the game database's title and
 * after the file's stem only for a game the database lacks; PerGame names it
 * by serial and PerGameFileTitle by stem. Card2Type defaults to None. */
#define DUCK_CARD(slot, role, title_default)                                                     \
    M_OPT("memcards/{stem}_" slot ".mcd", role, "Card" slot "Type", "PerGameTitle", title_default), \
    M_OPT("memcards/{title_id}_" slot ".mcd", role, "Card" slot "Type", "PerGame", false),         \
    M_OPT("memcards/{stem}_" slot ".mcd", role, "Card" slot "Type", "PerGameFileTitle", false)
static const sigil_layout_member DUCKSTATION_MEMBERS[] = {
    DUCK_CARD("1", PRIMARY, true),
    DUCK_CARD("2", SIDECAR, false),
};
static const sigil_layout_shared DUCKSTATION_SHARED[] = {
    S_OPT("memcards/shared_card_1.mcd", "Card1Type", "Shared", false),
    S_OPT("memcards/shared_card_2.mcd", "Card2Type", "Shared", false),
};
static const char *const DUCKSTATION_SUBDIRS[] = { "memcards" };

/* ARMSX1 (ARMSX2/ARMSX1 frontend/main.cpp): two 128 KiB cards every game
 * shares, directly in the app's private files folder. */
static const sigil_layout_shared ARMSX1_SHARED[] = {
    S("slot1.mcd"),
    S("slot2.mcd"),
};

/* PS1 classics on a PSP, and on a Vita through its PSP emulator (official
 * PS1 Classics and Adrenaline): PSP/SAVEDATA/<DISC_ID>/ under ms0:/ or
 * ux0:pspemu/, slot 1 in SCEVMC0.VMP and slot 2 in SCEVMC1.VMP. DISC_ID is the
 * EBOOT's, the disc serial with its punctuation dropped (SLUS01040). */
static const sigil_layout_member VITA_POPS_MEMBERS[] = {
    M("PSP/SAVEDATA/{disc_id}/SCEVMC0.VMP", PRIMARY),
    M("PSP/SAVEDATA/{disc_id}/SCEVMC1.VMP", SIDECAR),
};
static const char *const VITA_POPS_SUBDIRS[] = { "PSP/SAVEDATA" };

static const sigil_layout_member LRPS2_MEMBERS[] = {
    M_OPT("{stem}.ps2", PRIMARY, "pcsx2_shared_memory_cards", "disabled", false),
};
static const sigil_layout_shared LRPS2_SHARED[] = {
    S_OPT("Mcd001.ps2", "pcsx2_shared_memory_cards", "enabled", true),
    S_OPT("Mcd002.ps2", "pcsx2_shared_memory_cards", "enabled", true),
};

/* Standalone PCSX2 and its forks (AetherSX2, NetherSX2, ARMSX2), rooted at
 * the data folder that holds memcards/. Slots 1 and 2 default to Mcd001.ps2
 * and Mcd002.ps2 (Pcsx2Config.cpp, MemoryCardFile.cpp); each is a file card,
 * or a folder card when the name is a directory (MemoryCardFolder.cpp). */
static const sigil_layout_shared PCSX2_STANDALONE_SHARED[] = {
    S("memcards/Mcd001.ps2"),
    S("memcards/Mcd002.ps2"),
};
static const char *const PCSX2_STANDALONE_SUBDIRS[] = { "memcards" };

static const sigil_layout_member BEETLE_NGP_MEMBERS[] = {
    M("{stem}.flash", PRIMARY),
};

static const sigil_layout_member OPERA_MEMBERS[] = {
    M_OPT("opera/per_game/{stem}.{nvram_version}.srm", PRIMARY, "opera_nvram_storage", "per game", true),
};
static const sigil_layout_shared OPERA_SHARED[] = {
    S_OPT("opera/shared/nvram.{nvram_version}.srm", "opera_nvram_storage", "shared", false),
};
static const char *const OPERA_SUBDIRS[] = { "opera/per_game", "opera/shared" };

static const sigil_layout_member POKEMINI_MEMBERS[] = {
    M("{stem}.eep", PRIMARY),
};

static const sigil_layout_member HANDY_MEMBERS[] = {
    M("{stem}.eeprom", PRIMARY),
};

static const sigil_layout_member MELONDS_MEMBERS[] = {
    M("{stem}.sav", PRIMARY),
};

static const sigil_layout_member FBNEO_MEMBERS[] = {
    M("fbneo/{romset}.fs", PRIMARY),
    M("fbneo/{romset}.nv", SIDECAR),
    M_OPT("fbneo/{romset}.memcard", SIDECAR, "fbneo-memcard-mode", "per-game", false),
};
static const sigil_layout_shared FBNEO_SHARED[] = {
    S_OPT("fbneo/shared.memcard", "fbneo-memcard-mode", "shared", false),
};
static const char *const FBNEO_SUBDIRS[] = { "fbneo" };

static const sigil_layout_member MAME2003_PLUS_MEMBERS[] = {
    M_OPT("mame2003-plus/nvram/{romset}.nv", PRIMARY, "mame2003-plus_core_save_subfolder", "enabled", true),
    M_OPT("mame2003-plus/hi/{romset}.hi", SIDECAR, "mame2003-plus_core_save_subfolder", "enabled", true),
    M_OPT("nvram/{romset}.nv", PRIMARY, "mame2003-plus_core_save_subfolder", "disabled", false),
    M_OPT("hi/{romset}.hi", SIDECAR, "mame2003-plus_core_save_subfolder", "disabled", false),
};
static const char *const MAME2003_PLUS_SUBDIRS[] = {
    "mame2003-plus/nvram", "mame2003-plus/hi", "nvram", "hi"
};

static const sigil_layout_member DOSBOX_PURE_MEMBERS[] = {
    M("{stem}.pure.zip", PRIMARY),
};

static const sigil_layout_member SAME_CDI_MEMBERS[] = {
    M_OPT("same_cdi/nvram/{stem}/", PRIMARY, "same_cdi_nvram_saves", "enabled", true),
};
static const char *const SAME_CDI_SUBDIRS[] = { "same_cdi/nvram" };

/* Nestopia (libretro/libretro.cpp SAVE_FDS): a disk has no save RAM, so
 * RetroArch writes no .srm; the core writes a patch against the loaded file.
 * Builds before the format option (3ac52e67c4) always wrote .sav as UPS. */
static const sigil_layout_member NESTOPIA_FDS_MEMBERS[] = {
    M_OPT("{stem}.sav", PRIMARY, "nestopia_fds_savefile_format", "sav_ups", true),
    M_OPT("{stem}.ups", PRIMARY, "nestopia_fds_savefile_format", "ups", false),
    M_OPT("{stem}.ips", PRIMARY, "nestopia_fds_savefile_format", "ips", false),
};

/* bsnes (target-libretro/program.cpp openRomSuperFamicom): the core writes
 * save.ram, a clock cart's time.rtc (sfc/cartridge/save.cpp saveEpsonRTC,
 * saveSharpRTC) and the Satellaview cart's download.ram (saveMCC) itself. */
static const sigil_layout_member BSNES_SNES_MEMBERS[] = {
    M("{stem}.srm", PRIMARY),
    M("{stem}.rtc", RTC),
    M("{stem}.psr", SIDECAR),
};

/* yuzu and its forks (legacy layout, EDEN src/core/file_sys/savedata_factory.cpp
 * GetFullPath): nand/user/save/0000000000000000/<user folder>/<TITLEID>/,
 * device saves under the all-zero user. Each fork names its size file after
 * itself (savedata_factory.h); a restore leaves the target's own in place. */
static const sigil_layout_area YUZU_AREAS[] = {
    { "nand/user/save/0000000000000000/{profile}/{save_id}/", "{save_id}/", SIGIL_SAVE_AREA_ACCOUNT, false, NULL },
    { "nand/user/save/0000000000000000/00000000000000000000000000000000/{save_id}/", "device/{save_id}/",
      SIGIL_SAVE_AREA_DEVICE, false, NULL },
};
static const char *const YUZU_IGNORED[] = {
    ".yuzu_save_size", ".citron_save_size", ".sudachi_save_size", ".suyu_save_size",
};
static const sigil_layout_profiles YUZU_PROFILES = {
    "nand", SIGIL_PROFILES_YUZU, "nand/system/save/8000000000000010/su/avators/profiles.dat",
    YUZU_AREAS, COUNT(YUZU_AREAS), YUZU_IGNORED, COUNT(YUZU_IGNORED),
};
static const char *const YUZU_SUBDIRS[] = {
    "nand/user/save/0000000000000000", "nand/system/save/8000000000000010/su/avators",
};

/* Skyline and its continuation Strato (services/fssrv/IFileSystemProxy.cpp,
 * services/account/IAccountServiceForApplication.h): yuzu's tree under
 * switch/nand/, with one user every game runs as and no profile list. A unit
 * names its folders as the yuzu forks' does. */
static const sigil_layout_area SKYLINE_AREAS[] = {
    { "switch/nand/user/save/0000000000000000/{profile}/{save_id}/", "{save_id}/", SIGIL_SAVE_AREA_ACCOUNT, false,
      NULL },
    { "switch/nand/user/save/0000000000000000/00000000000000000000000000000000/{save_id}/", "device/{save_id}/",
      SIGIL_SAVE_AREA_DEVICE, false, NULL },
};
static const sigil_layout_profiles SKYLINE_PROFILES = {
    "switch", 0, NULL, SKYLINE_AREAS, COUNT(SKYLINE_AREAS), NULL, 0, false, false, "00000000000000000000000000000001",
};
static const char *const SKYLINE_SUBDIRS[] = { "switch/nand/user/save/0000000000000000" };

/* Ryujinx and its forks Ryubing and Kenji-NX (LibHac SaveDataIndexer,
 * DirectorySaveDataFileSystem; Ryujinx.HLE Account/Acc): bis/user/save/<id>/
 * where <id> is the save index's id for the save, not the title, and 0/ holds
 * the committed save. The index (imkvdb.arc) says whose each folder is;
 * system/Profiles.json lists the users. */
static const sigil_layout_area RYUJINX_AREAS[] = {
    { "bis/user/save/{save_id}/0/", "{save_id}/", SIGIL_SAVE_AREA_DEVICE, false, NULL },
};
static const sigil_layout_profiles RYUJINX_PROFILES = {
    "bis", SIGIL_PROFILES_RYUJINX, "system/Profiles.json", RYUJINX_AREAS, COUNT(RYUJINX_AREAS), NULL, 0, false, false,
    NULL, "bis/system/save/8000000000000000/0/imkvdb.arc",
};
static const char *const RYUJINX_SUBDIRS[] = { "bis/user/save", "bis/system/save/8000000000000000", "system" };

/* Cemu (src/Cafe/TitleList/SaveInfo.cpp, Account.cpp): the mlc's
 * usr/save/00050000/<title low>/ holds meta/, user/<persistent id>/ per
 * account and user/common/ for every account. Cemu writes meta/ again when
 * the game opens its save. */
static const sigil_layout_area CEMU_AREAS[] = {
    { "mlc01/usr/save/00050000/{save_id}/meta/", "{save_id}/meta/", SIGIL_SAVE_AREA_DEVICE, true, NULL },
    { "mlc01/usr/save/00050000/{save_id}/user/common/", "{save_id}/user/common/", SIGIL_SAVE_AREA_DEVICE, false,
      NULL },
    { "mlc01/usr/save/00050000/{save_id}/user/{profile}/", "{save_id}/user/account/", SIGIL_SAVE_AREA_ACCOUNT, false,
      "{save_id}/user/{profile}/" },
};
static const sigil_layout_profiles CEMU_PROFILES = {
    "mlc01", SIGIL_PROFILES_CEMU, "mlc01/usr/save/system/act/{profile}/account.dat",
    CEMU_AREAS, COUNT(CEMU_AREAS), NULL, 0,
};
static const char *const CEMU_SUBDIRS[] = { "mlc01/usr/save/00050000", "mlc01/usr/save/system/act" };

/* Vita3K (vita3k/io/src/io.cpp): ux0/user/<user id>/savedata/<SAVEDIR>/. */
static const sigil_layout_area VITA3K_AREAS[] = {
    { "ux0/user/{profile}/savedata/{save_id}/", "{save_id}/", SIGIL_SAVE_AREA_ACCOUNT, false, NULL },
};
static const sigil_layout_profiles VITA3K_PROFILES = {
    "ux0", SIGIL_PROFILES_VITA3K, "ux0/user/{profile}/user.xml", VITA3K_AREAS, COUNT(VITA3K_AREAS), NULL, 0,
};
static const char *const VITA3K_SUBDIRS[] = { "ux0/user" };

/* RPCS3 (rpcs3/Emu/Cell/Modules/cellSaveData.cpp): dev_hdd0/home/<user id>/
 * savedata/<DIRNAME>/, every DIRNAME the game writes starting with its
 * title id. */
static const sigil_layout_area RPCS3_AREAS[] = {
    { "dev_hdd0/home/{profile}/savedata/{save_id}/", "{save_id}/", SIGIL_SAVE_AREA_ACCOUNT, false, NULL },
};
static const sigil_layout_profiles RPCS3_PROFILES = {
    "dev_hdd0", SIGIL_PROFILES_RPCS3, "dev_hdd0/home/{profile}/localusername", RPCS3_AREAS, COUNT(RPCS3_AREAS),
    NULL, 0, true,
};
static const char *const RPCS3_SUBDIRS[] = { "dev_hdd0/home" };

/* mupen64plus 2.6+, RMG and simple64 (mupen64plus-core src/main/main.c
 * get_save_filename): {goodname[:32]}-{MD5[:8]} from mupen64plus.ini, or the
 * header name for a ROM the database lacks, so the hash suffix is what sigil
 * can match. One .mpk holds all four controller paks. */
static const sigil_layout_member MUPEN64PLUS_MEMBERS[] = {
    M("*-{n64_md5_8}.eep", PRIMARY),
    M("*-{n64_md5_8}.sra", PRIMARY),
    M("*-{n64_md5_8}.fla", PRIMARY),
    M("*-{n64_md5_8}.mpk", SIDECAR),
};

/* M64Plus FZ (mupen64plus-ae GamePrefs.java getGameDataPath, setGameDirs):
 * GameData/{header} {country} {md5}/SramData/, or GameData/{md5}/ where the
 * file system can't hold that name; the files inside carry FZ's database
 * name. The client reaches the copy FZ keeps in the user's chosen folder. */
static const sigil_layout_member M64PLUS_FZ_MEMBERS[] = {
    M("GameData/*{n64_md5_lower}/SramData/*.eep", PRIMARY),
    M("GameData/*{n64_md5_lower}/SramData/*.sra", PRIMARY),
    M("GameData/*{n64_md5_lower}/SramData/*.fla", PRIMARY),
    M("GameData/*{n64_md5_lower}/SramData/*.mpk", SIDECAR),
};
static const char *const M64PLUS_FZ_SUBDIRS[] = { "GameData" };

/* Project64 (N64Rom.cpp SaveRomSettingID, SaveType/Eeprom.cpp and its siblings, Mempak.cpp):
 * Save/{header}-{MD5 of the .n64-order ROM}/ with "Unique Game Dir" on (the
 * default), Save/ itself with it off; one .mpk per controller. */
#define PJ64_UNIQUE(t, r) M_OPT("Save/*-{n64_md5_n64}/" t, r, "Unique Game Dir", "1", true)
#define PJ64_FLAT(t, r)   M_OPT("Save/{n64_header}" t, r, "Unique Game Dir", "0", false)
static const sigil_layout_member PROJECT64_MEMBERS[] = {
    PJ64_UNIQUE("*.eep", PRIMARY), PJ64_UNIQUE("*.sra", PRIMARY), PJ64_UNIQUE("*.fla", PRIMARY),
    PJ64_UNIQUE("*_Cont_1.mpk", SIDECAR), PJ64_UNIQUE("*_Cont_2.mpk", SIDECAR), PJ64_UNIQUE("*_Cont_3.mpk", SIDECAR),
    PJ64_UNIQUE("*_Cont_4.mpk", SIDECAR),
    PJ64_FLAT(".eep", PRIMARY), PJ64_FLAT(".sra", PRIMARY), PJ64_FLAT(".fla", PRIMARY),
    PJ64_FLAT("_Cont_1.mpk", SIDECAR), PJ64_FLAT("_Cont_2.mpk", SIDECAR), PJ64_FLAT("_Cont_3.mpk", SIDECAR),
    PJ64_FLAT("_Cont_4.mpk", SIDECAR),
};
static const char *const PROJECT64_SUBDIRS[] = { "Save" };

/* PSP games on PPSSPP (Core/Dialog/SavedataParam.cpp, ms0:/PSP/SAVEDATA/),
 * whose libretro core makes the frontend's save folder the memory stick
 * (libretro/libretro.cpp), and on a PSP or a Vita's PSP emulator (ux0:pspemu/).
 * A game names each of its folders with its disc id and a suffix of its own.
 * Game-data installs (PSPGamedataInstallDialog.cpp) share the folder and the
 * prefix; their PARAM.SFO lacks the hash keys every save's carries
 * (SavedataParam.cpp), which is how they are left out. */
static const sigil_layout_area PSP_AREAS[] = {
    { "PSP/SAVEDATA/{save_id}/", "{save_id}/", SIGIL_SAVE_AREA_DEVICE, false, NULL },
};
static const sigil_layout_profiles PSP_FOLDERS = {
    "PSP", 0, NULL, PSP_AREAS, COUNT(PSP_AREAS), NULL, 0, true, true,
};
static const char *const PSP_SUBDIRS[] = { "PSP/SAVEDATA" };

#define ROW_PROFILES(l, p, d, pr) { l, p, NULL, 0, NULL, 0, d, COUNT(d), NULL, &pr }

#define ROW(l, p, m)            { l, p, m, COUNT(m), NULL, 0, NULL, 0 }
#define ROW_SHARED(l, p, m, s)  { l, p, m, COUNT(m), s, COUNT(s), NULL, 0 }
#define ROW_FULL(l, p, m, s, d) { l, p, m, COUNT(m), s, COUNT(s), d, COUNT(d) }
#define ROW_DIRS(l, p, m, d)    { l, p, m, COUNT(m), NULL, 0, d, COUNT(d) }
#define ROW_REGION(l, p, m, s, r) { l, p, m, COUNT(m), s, COUNT(s), NULL, 0, r }

static const sigil_layout LIBRETRO_DEFAULT = ROW("libretro", NULL, LIBRETRO_DEFAULT_MEMBERS);

static const sigil_layout LAYOUTS[] = {
    ROW("vba_next", NULL, SRM_ONLY_MEMBERS),
    ROW("gpsp", NULL, SRM_ONLY_MEMBERS),
    ROW("bsnes", "snes", BSNES_SNES_MEMBERS),
    ROW_REGION("genesis_plus_gx", "segacd", GPGX_SEGACD_MEMBERS, GPGX_SEGACD_SHARED, "genesis_plus_gx_region_detect"),
    ROW_SHARED("mednafen_psx_hw", NULL, BEETLE_PSX_HW_MEMBERS, BEETLE_PSX_HW_SHARED),
    ROW_SHARED("mednafen_psx", NULL, BEETLE_PSX_MEMBERS, BEETLE_PSX_SHARED),
    ROW_SHARED("pcsx_rearmed", NULL, PCSX_REARMED_MEMBERS, PCSX_REARMED_SHARED),
    ROW_DIRS("vita_pops", "psx", VITA_POPS_MEMBERS, VITA_POPS_SUBDIRS),
    ROW_SHARED("swanstation", "psx", SWANSTATION_MEMBERS, SWANSTATION_SHARED),
    ROW_FULL("duckstation", "psx", DUCKSTATION_MEMBERS, DUCKSTATION_SHARED, DUCKSTATION_SUBDIRS),
    { "armsx1", "psx", NULL, 0, ARMSX1_SHARED, COUNT(ARMSX1_SHARED), NULL, 0, NULL },
    ROW_SHARED("pcsx2", NULL, LRPS2_MEMBERS, LRPS2_SHARED),
    { "pcsx2_standalone", "ps2", NULL, 0, PCSX2_STANDALONE_SHARED, COUNT(PCSX2_STANDALONE_SHARED),
      PCSX2_STANDALONE_SUBDIRS, COUNT(PCSX2_STANDALONE_SUBDIRS), NULL },
    ROW_SHARED("mednafen_saturn", NULL, BEETLE_SATURN_MEMBERS, BEETLE_SATURN_SHARED),
    ROW_DIRS("kronos", "saturn", KRONOS_MEMBERS, KRONOS_SUBDIRS),
    { "dolphin", "gamecube", NULL, 0, DOLPHIN_SHARED, COUNT(DOLPHIN_SHARED), DOLPHIN_SUBDIRS, COUNT(DOLPHIN_SUBDIRS), NULL },
    { "dolphin_standalone", "gamecube", NULL, 0, DOLPHIN_STANDALONE_SHARED, COUNT(DOLPHIN_STANDALONE_SHARED),
      DOLPHIN_STANDALONE_SUBDIRS, COUNT(DOLPHIN_STANDALONE_SUBDIRS), NULL },
    ROW_SHARED("flycast", "dreamcast", FLYCAST_MEMBERS, FLYCAST_SHARED),
    ROW_SHARED("flycast_standalone", "dreamcast", FLYCAST_STANDALONE_MEMBERS, FLYCAST_STANDALONE_SHARED),
    ROW("yabause", "saturn", YABAUSE_MEMBERS),
    { "yabasanshiro", "saturn", NULL, 0, YABASANSHIRO_SHARED, COUNT(YABASANSHIRO_SHARED),
      YABASANSHIRO_SUBDIRS, COUNT(YABASANSHIRO_SUBDIRS), NULL },
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
    /* mupen64plus_next and parallel_n64 (libretro/libretro_memory.h) keep
     * EEPROM, four controller paks, SRAM and flash in one 0x48800-byte .srm. */
    ROW("mupen64plus_next", "n64", SRM_ONLY_MEMBERS),
    ROW("parallel_n64", "n64", SRM_ONLY_MEMBERS),
    ROW("mupen64plus_standalone", "n64", MUPEN64PLUS_MEMBERS),
    ROW_DIRS("m64plus_fz", "n64", M64PLUS_FZ_MEMBERS, M64PLUS_FZ_SUBDIRS),
    ROW_DIRS("project64", "n64", PROJECT64_MEMBERS, PROJECT64_SUBDIRS),
    ROW_PROFILES("eden", "switch", YUZU_SUBDIRS, YUZU_PROFILES),
    ROW_PROFILES("citron", "switch", YUZU_SUBDIRS, YUZU_PROFILES),
    ROW_PROFILES("sudachi", "switch", YUZU_SUBDIRS, YUZU_PROFILES),
    ROW_PROFILES("yuzu", "switch", YUZU_SUBDIRS, YUZU_PROFILES),
    ROW_PROFILES("lemon", "switch", YUZU_SUBDIRS, YUZU_PROFILES),
    ROW_PROFILES("skyline", "switch", SKYLINE_SUBDIRS, SKYLINE_PROFILES),
    ROW_PROFILES("strato", "switch", SKYLINE_SUBDIRS, SKYLINE_PROFILES),
    ROW_PROFILES("ryujinx", "switch", RYUJINX_SUBDIRS, RYUJINX_PROFILES),
    ROW_PROFILES("kenjinx", "switch", RYUJINX_SUBDIRS, RYUJINX_PROFILES),
    ROW_PROFILES("cemu", "wiiu", CEMU_SUBDIRS, CEMU_PROFILES),
    ROW_PROFILES("vita3k", "psvita", VITA3K_SUBDIRS, VITA3K_PROFILES),
    ROW_PROFILES("rpcs3", "ps3", RPCS3_SUBDIRS, RPCS3_PROFILES),
    /* aPS3e (aenu1/aps3e) and ARMSX3 keep RPCS3's tree under their config/ folder, user 00000001. */
    ROW_PROFILES("aps3e", "ps3", RPCS3_SUBDIRS, RPCS3_PROFILES),
    ROW_PROFILES("armsx3", "ps3", RPCS3_SUBDIRS, RPCS3_PROFILES),
    ROW_PROFILES("ppsspp", "psp", PSP_SUBDIRS, PSP_FOLDERS),
    ROW_PROFILES("ppsspp_standalone", "psp", PSP_SUBDIRS, PSP_FOLDERS),
    ROW_PROFILES("psp_console", "psp", PSP_SUBDIRS, PSP_FOLDERS),
};

const char *sigil_layout_platform(const char *slug) {
    if (!slug) return NULL;
    if (strcmp(slug, "scd") == 0 || strcmp(slug, "sega_cd") == 0 || strcmp(slug, "sega-cd") == 0
        || strcmp(slug, "mega_cd") == 0 || strcmp(slug, "mega-cd") == 0 || strcmp(slug, "megacd") == 0) {
        return "segacd";
    }
    if (strcmp(slug, "sfc") == 0 || strcmp(slug, "sfam") == 0) return "snes";
    if (strcmp(slug, "ps1") == 0 || strcmp(slug, "playstation") == 0) return "psx";
    if (strcmp(slug, "famicom_disk_system") == 0 || strcmp(slug, "fds") == 0) return "fds";
    if (strcmp(slug, "dc") == 0) return "dreamcast";
    if (strcmp(slug, "ngc") == 0 || strcmp(slug, "gc") == 0) return "gamecube";
    if (strcmp(slug, "vita") == 0) return "psvita";
    return slug;
}

static bool platform_matches(const char *row_platform, const char *platform) {
    if (!row_platform) return true;
    if (!platform) return false;
    return strcmp(row_platform, sigil_layout_platform(platform)) == 0;
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
