// SPDX-License-Identifier: MPL-2.0
#include "save_corpus.h"
#include "mem_root.h"
#include "card_saturn.h"
#include "card_segacd.h"
#include <stdbool.h>

#define TEST_SKIP  77
#define LUNAR      "Lunar - The Silver Star (USA).cue"
#define LUNAR_BRM  "Lunar - The Silver Star (USA).brm"
#define RAYMAN     "Rayman (USA) (R2)"
#define MAX_OPTS   2

static int g_fails = 0;

static void fail(const char *where, const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", where, what);
    g_fails++;
}

static corpus_table g_saturn, g_segacd;

/* ---- samples ------------------------------------------------------------------ */

static uint8_t *sample(const corpus_table *manifest, const char *platform, const char *id, const char *path, size_t *len) {
    return corpus_sample(manifest, platform, id, path, len);
}

static uint8_t *segacd(const char *id, size_t *len) { return sample(&g_segacd, "segacd", id, NULL, len); }

static sigil_card_listing *listing_of(const uint8_t *data, size_t len) {
    sigil_io *io = mem_root_io(data, len);
    sigil_card_listing *l = NULL;
    if (sigil_card_list(io, &l) != SIGIL_OK) l = NULL;
    sigil_io_close(io);
    return l;
}

static bool has_name(const sigil_card_listing *l, const char *name) {
    for (size_t i = 0; l && i < l->entry_count; i++) {
        if (strcmp(l->entries[i].name, name) == 0) return true;
    }
    return false;
}

static size_t entry_count(const uint8_t *data, size_t len) {
    sigil_card_listing *l = listing_of(data, len);
    size_t n = l ? l->entry_count : 0;
    sigil_card_listing_free(l);
    return n;
}

/* One Sega CD save lifted off a volume, as a .scd unit. */
static uint8_t *segacd_save(const uint8_t *volume, size_t len, const char *name, size_t *unit_len) {
    sigil_io *io = mem_root_io(volume, len);
    sigil_segacd_volume v;
    uint8_t *unit = NULL;
    if (sigil_segacd_volume_load(io, &v) == SIGIL_OK) {
        sigil_card_listing *l = NULL;
        if (sigil_segacd_list(&v, &l) == SIGIL_OK) {
            for (size_t i = 0; i < l->entry_count && !unit; i++) {
                if (strcmp(l->entries[i].name, name) != 0) continue;
                *unit_len = sigil_segacd_unit_size(l->entries[i].blocks);
                unit = (uint8_t *)malloc(*unit_len);
                if (sigil_segacd_extract(&v, l->entries[i].first_block, l->entries[i].blocks, unit) != SIGIL_OK) {
                    free(unit);
                    unit = NULL;
                }
            }
            sigil_card_listing_free(l);
        }
        sigil_segacd_volume_free(&v);
    }
    sigil_io_close(io);
    return unit;
}

/* `volume` with the .scd `unit` added, written back in the file's form. */
static uint8_t *segacd_with(const uint8_t *volume, size_t len, const uint8_t *unit, size_t unit_len, size_t *out_len) {
    sigil_io *io = mem_root_io(volume, len);
    sigil_segacd_volume v;
    uint8_t *out = NULL;
    if (sigil_segacd_volume_load(io, &v) == SIGIL_OK) {
        if (sigil_segacd_inject(&v, unit, unit_len) != SIGIL_OK || sigil_segacd_volume_write(&v, &out, out_len) != SIGIL_OK) out = NULL;
        sigil_segacd_volume_free(&v);
    }
    sigil_io_close(io);
    return out;
}

/* `volume` with the save `name` replaced by the .scd `unit`. */
static uint8_t *segacd_replaced(const uint8_t *volume, size_t len, const char *name, const uint8_t *unit, size_t unit_len,
                                size_t *out_len) {
    sigil_io *io = mem_root_io(volume, len);
    sigil_segacd_volume v;
    uint8_t *out = NULL;
    if (sigil_segacd_volume_load(io, &v) == SIGIL_OK) {
        sigil_card_listing *l = NULL;
        bool removed = false;
        if (sigil_segacd_list(&v, &l) == SIGIL_OK) {
            for (size_t i = 0; i < l->entry_count && !removed; i++) {
                if (strcmp(l->entries[i].name, name) == 0) removed = sigil_segacd_delete(&v, l->entries[i].first_block) == SIGIL_OK;
            }
            sigil_card_listing_free(l);
        }
        if (!removed || sigil_segacd_inject(&v, unit, unit_len) != SIGIL_OK || sigil_segacd_volume_write(&v, &out, out_len) != SIGIL_OK) {
            out = NULL;
        }
        sigil_segacd_volume_free(&v);
    }
    sigil_io_close(io);
    return out;
}

/* ---- requests ----------------------------------------------------------------- */

typedef struct {
    sigil_sync_request req;
    sigil_save_option  options[MAX_OPTS];
} game;

static void make_game(game *g, mem_root *root, const char *layout, const char *platform, const char *content, int mode) {
    memset(g, 0, sizeof(*g));
    g->req.struct_version = SIGIL_SYNC_REQUEST_V1;
    g->req.save.struct_version = SIGIL_SAVE_REQUEST_V1;
    g->req.save.layout = layout;
    g->req.save.platform = platform;
    g->req.save.content_path = content;
    g->req.save.listing = root->listing;
    g->req.save.listing_count = root->count;
    g->req.save.open = root_open;
    g->req.save.open_ctx = root;
    g->req.write = root_write;
    g->req.write_ctx = root;
    g->req.mode = mode;
}

static void add_option(game *g, const char *key, const char *value) {
    g->options[g->req.save.option_count].key = key;
    g->options[g->req.save.option_count].value = value;
    g->req.save.option_count++;
    g->req.save.options = g->options;
}

static void refresh(game *g, mem_root *root) {
    g->req.save.listing = root->listing;
    g->req.save.listing_count = root->count;
}

static void use_state(game *g, const sigil_sync_result *r) {
    g->req.state = r ? r->state : NULL;
    g->req.state_len = r ? r->state_len : 0;
}

/* Collects Lunar's save from a per-game volume: the unit other checks restore. */
static sigil_sync_result *lunar_unit(void) {
    size_t len = 0;
    uint8_t *vol = segacd("lunar-ecc-brm", &len);
    if (!vol) return NULL;
    mem_root root = {0};
    root_put(&root, LUNAR_BRM, vol, len);
    game g;
    make_game(&g, &root, "genesis_plus_gx", "segacd", LUNAR, SIGIL_SYNC_MANAGED);
    add_option(&g, "genesis_plus_gx_system_bram", "per game");
    sigil_sync_result *r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK || !r->data) { fail("lunar unit", "collect from its own volume failed"); sigil_sync_result_free(r); r = NULL; }
    root_free(&root);
    free(vol);
    return r;
}

static size_t last_set_byte(const uint8_t *image, size_t len) {
    size_t at = len;
    while (at > 0 && image[at - 1] == 0) at--;
    return at ? at - 1 : 0;
}

static size_t first_set_byte(const uint8_t *image, size_t len) {
    size_t at = 0;
    while (at < len && image[at] == 0) at++;
    return at;
}

/* A per-game volume that reads back with a byte of a save's data wrong fails
 * the restore. `data_at` finds a byte of save data in the cleanly written
 * volume. */
static void check_faulty_write(const char *where, const char *layout, const char *platform, const char *content,
                               const char *opt_key, const char *opt_value, const uint8_t *unit, size_t unit_len,
                               size_t (*data_at)(const uint8_t *, size_t)) {
    mem_root clean = {0}, faulty = {0};
    game g;
    make_game(&g, &clean, layout, platform, content, SIGIL_SYNC_MANAGED);
    if (opt_key) add_option(&g, opt_key, opt_value);
    sigil_sync_result *r = NULL, *bad = NULL;
    if (sigil_restore(&g.req, unit, unit_len, &r) != SIGIL_OK || clean.count != 1) {
        fail(where, "clean restore failed");
    } else {
        faulty.corrupt_write = 1;
        faulty.corrupt_at = data_at(clean.files[0].data, clean.files[0].len);
        make_game(&g, &faulty, layout, platform, content, SIGIL_SYNC_MANAGED);
        if (opt_key) add_option(&g, opt_key, opt_value);
        if (sigil_restore(&g.req, unit, unit_len, &bad) != SIGIL_ERR_IO) fail(where, "a volume that read back wrong passed the restore");
    }
    sigil_sync_result_free(bad);
    sigil_sync_result_free(r);
    root_free(&faulty);
    root_free(&clean);
}

/* ---- Saturn ------------------------------------------------------------------- */

/* A game's internal and cart volumes travel together as a zip, and a save with
 * the same name on both stays two saves. */
static void check_saturn_internal_and_cart(void) {
    size_t int_len = 0, cart_len = 0;
    uint8_t *internal = sample(&g_saturn, "saturn", "rayman-bkr-bcr", "Rayman (USA) (R2)-internal.bkr", &int_len);
    uint8_t *cart = sample(&g_saturn, "saturn", "rayman-bkr-bcr", "Rayman (USA) (R2)-cart.bcr", &cart_len);
    if (!internal || !cart) { free(internal); free(cart); fail("saturn pair", "samples missing"); return; }
    mem_root root = {0};
    root_put(&root, RAYMAN ".srm", internal, int_len);
    root_put(&root, RAYMAN ".bcr", cart, cart_len);
    game g;
    make_game(&g, &root, "mednafen_saturn", "saturn", RAYMAN ".cue", SIGIL_SYNC_MANAGED);
    sigil_sync_result *r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK || !r->data) {
        fail("saturn pair", "collect failed");
    } else {
        if (r->shape != SIGIL_SAVE_SHAPE_MULTI || strcmp(r->artifact, RAYMAN ".zip") != 0) fail("saturn pair", "not a zip named for the content");
        if (r->holding || r->unowned_count) fail("saturn pair", "per-game volumes held saves back");
        sigil_zip_member *m = NULL;
        size_t n = 0;
        if (sigil_zip_read_mem(r->data, r->len, 1u << 24, &m, &n) != SIGIL_OK || n != 2 ||
            strcmp(m[0].name, "backup.ram") != 0 || strcmp(m[1].name, "cart.ram") != 0) {
            fail("saturn pair", "zip doesn't hold backup.ram and cart.ram");
        } else {
            if (m[0].len != 32768 || entry_count(m[0].data, m[0].len) != entry_count(internal, int_len)) fail("saturn pair", "internal volume lost saves");
            if (entry_count(m[1].data, m[1].len) != entry_count(cart, cart_len)) fail("saturn pair", "cart volume lost saves");
            sigil_named_md5 parts[2];
            for (size_t i = 0; i < 2; i++) {
                snprintf(parts[i].name, sizeof(parts[i].name), "%s", m[i].name);
                sigil_md5_of(m[i].data, m[i].len, parts[i].md5);
            }
            char expect[33];
            sigil_named_hash(parts, 2, expect);
            if (strcmp(expect, r->content_hash) != 0) fail("saturn pair", "content hash isn't RomM's zip hash");
        }
        sigil_zip_members_free(m, n);

        mem_root no_cart = {0};
        game yabause;
        make_game(&yabause, &no_cart, "yabause", "saturn", RAYMAN ".cue", SIGIL_SYNC_MANAGED);
        sigil_sync_result *refused = NULL;
        if (sigil_restore(&yabause.req, r->data, r->len, &refused) != SIGIL_ERR_NO_TARGET || !refused ||
            strcmp(refused->problem, "cart.ram") != 0 || no_cart.writes != 0) {
            fail("saturn pair", "a cart volume restored to a core with no cart file didn't refuse naming it");
        }
        sigil_sync_result_free(refused);
        root_free(&no_cart);

        mem_root empty = {0};
        game fresh;
        make_game(&fresh, &empty, "mednafen_saturn", "saturn", RAYMAN ".cue", SIGIL_SYNC_MANAGED);
        sigil_sync_result *restored = NULL, *back = NULL;
        if (sigil_restore(&fresh.req, r->data, r->len, &restored) != SIGIL_OK || empty.writes != 2) {
            fail("saturn pair", "restore into an empty root didn't write both volumes");
        } else {
            refresh(&fresh, &empty);
            use_state(&fresh, restored);
            if (sigil_collect(&fresh.req, &back) != SIGIL_OK || strcmp(back->identity_hash, r->identity_hash) != 0 || back->changed) {
                fail("saturn pair", "saves differ after the trip, or read as changed");
            }
        }
        sigil_sync_result_free(back);
        sigil_sync_result_free(restored);
        root_free(&empty);
    }
    sigil_sync_result_free(r);
    root_free(&root);
    free(internal);
    free(cart);
}

/* A volume standalone Mednafen gzipped goes back gzipped. */
static void check_saturn_keeps_gzip(void) {
    size_t int_len = 0, cart_len = 0;
    uint8_t *internal = sample(&g_saturn, "saturn", "sf3-scn3-bkr", NULL, &int_len);
    uint8_t *cart = sample(&g_saturn, "saturn", "sf3-scn3-bcr", "Shining Force III Scenario 3 (English v25.1).bcr", &cart_len);
    if (!internal || !cart || cart[0] != 0x1F) { free(internal); free(cart); fail("saturn gzip", "samples missing"); return; }
    const char *stem = "Shining Force III Scenario 3 (English v25.1)";
    char srm[256], bcr[256], cue[256];
    snprintf(srm, sizeof(srm), "%s.srm", stem);
    snprintf(bcr, sizeof(bcr), "%s.bcr", stem);
    snprintf(cue, sizeof(cue), "%s.cue", stem);
    mem_root root = {0};
    root_put(&root, srm, internal, int_len);
    root_put(&root, bcr, cart, cart_len);
    game g;
    make_game(&g, &root, "mednafen_saturn", "saturn", cue, SIGIL_SYNC_MANAGED);
    sigil_sync_result *r = NULL, *again = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK || !r->data) {
        fail("saturn gzip", "collect failed");
    } else {
        mem_root cart_only = {0};
        root_put(&cart_only, bcr, cart, cart_len);
        game h;
        make_game(&h, &cart_only, "mednafen_saturn", "saturn", cue, SIGIL_SYNC_MANAGED);
        h.req.overwrite_local = 1;
        if (sigil_restore(&h.req, r->data, r->len, &again) != SIGIL_OK || cart_only.writes != 2) {
            fail("saturn gzip", "restore failed");
        } else {
            mem_file *f = root_find(&cart_only, bcr);
            if (f->len < 2 || f->data[0] != 0x1F || f->data[1] != 0x8B) fail("saturn gzip", "the cart lost its gzip form");
            if (entry_count(f->data, f->len) != entry_count(cart, cart_len)) fail("saturn gzip", "the cart lost saves");
        }
        root_free(&cart_only);
    }
    sigil_sync_result_free(again);
    sigil_sync_result_free(r);
    root_free(&root);
    free(internal);
    free(cart);
}

/* One volume travels as that volume, named for its device. */
static void check_saturn_single(void) {
    size_t len = 0;
    uint8_t *internal = sample(&g_saturn, "saturn", "hyper-duel-bkr", NULL, &len);
    if (!internal) return;
    mem_root root = {0};
    root_put(&root, "Hyper Duel (Japan).srm", internal, len);
    game g;
    make_game(&g, &root, "mednafen_saturn", "saturn", "Hyper Duel (Japan).cue", SIGIL_SYNC_MANAGED);
    sigil_sync_result *r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK || !r->data || r->shape != SIGIL_SAVE_SHAPE_SINGLE ||
        strcmp(r->artifact, "backup.ram") != 0 || r->len != 32768) {
        fail("saturn single", "an internal-only game isn't one backup.ram");
    } else {
        check_faulty_write("saturn faulty write", "mednafen_saturn", "saturn", "Hyper Duel (Japan).cue", NULL, NULL,
                           r->data, r->len, last_set_byte);
    }
    sigil_sync_result_free(r);
    root_free(&root);
    free(internal);
}

/* A core older than the save-method option keeps only `.bkr`: collect with no
 * options finds no saves but names the option value that reads them, and
 * collect and restore with that value take the `.bkr`. */
static void check_saturn_legacy_bkr(void) {
    size_t len = 0;
    uint8_t *internal = sample(&g_saturn, "saturn", "hyper-duel-bkr", NULL, &len);
    if (!internal) return;
    mem_root root = {0};
    root_put(&root, "Hyper Duel (Japan).bkr", internal, len);
    game g;
    make_game(&g, &root, "mednafen_saturn", "saturn", "Hyper Duel (Japan).cue", SIGIL_SYNC_MANAGED);
    sigil_sync_result *r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK || r->data) {
        fail("saturn legacy bkr", "a modern collect took the .bkr");
    } else if (r->alternate_count != 1 || strcmp(r->alternates[0].path, "Hyper Duel (Japan).bkr") != 0 ||
               r->alternates[0].option_count != 1 ||
               strcmp(r->alternates[0].options[0].key, "beetle_saturn_save_method") != 0 ||
               strcmp(r->alternates[0].options[0].value, "mednafen") != 0) {
        fail("saturn legacy bkr", "collect didn't name the option that reads the .bkr");
    } else {
        add_option(&g, r->alternates[0].options[0].key, r->alternates[0].options[0].value);
        sigil_sync_result *legacy = NULL;
        if (sigil_collect(&g.req, &legacy) != SIGIL_OK || !legacy->data || legacy->alternate_count != 0) {
            fail("saturn legacy bkr", "the named option didn't read the .bkr");
        } else {
            mem_root fresh_root = {0};
            game fresh;
            make_game(&fresh, &fresh_root, "mednafen_saturn", "saturn", "Hyper Duel (Japan).cue", SIGIL_SYNC_MANAGED);
            add_option(&fresh, "beetle_saturn_save_method", "mednafen");
            sigil_sync_result *back = NULL;
            if (sigil_restore(&fresh.req, legacy->data, legacy->len, &back) != SIGIL_OK ||
                !root_find(&fresh_root, "Hyper Duel (Japan).bkr") || root_find(&fresh_root, "Hyper Duel (Japan).srm")) {
                fail("saturn legacy bkr", "restore with the named option didn't write the .bkr alone");
            }
            sigil_sync_result_free(back);
            root_free(&fresh_root);

            game modern;
            make_game(&modern, &root, "mednafen_saturn", "saturn", "Hyper Duel (Japan).cue", SIGIL_SYNC_MANAGED);
            back = NULL;
            if (sigil_restore(&modern.req, legacy->data, legacy->len, &back) != SIGIL_OK || back->alternate_count != 1 ||
                strcmp(back->alternates[0].path, "Hyper Duel (Japan).bkr") != 0) {
                fail("saturn legacy bkr", "a modern restore didn't report the .bkr it left alone");
            }
            sigil_sync_result_free(back);
        }
        sigil_sync_result_free(legacy);
    }
    sigil_sync_result_free(r);
    root_free(&root);
    free(internal);
}

/* A cart volume of `size` bytes holding Rayman's cart save, written raw. */
static uint8_t *rayman_cart_of(size_t size, size_t *len) {
    size_t cart_len = 0;
    uint8_t *cart = sample(&g_saturn, "saturn", "rayman-bkr-bcr", "Rayman (USA) (R2)-cart.bcr", &cart_len);
    uint8_t *out = NULL;
    sigil_io *io = cart ? mem_root_io(cart, cart_len) : NULL;
    sigil_saturn_volume src, big;
    if (io && sigil_saturn_volume_load_cart(io, &src) == SIGIL_OK) {
        sigil_card_listing *l = NULL;
        uint8_t *bup = NULL;
        size_t bup_len = 0;
        sigil_bram_storage raw;
        memset(&raw, 0, sizeof(raw));
        raw.filler = -1;
        if (sigil_saturn_list(&src, &l) == SIGIL_OK && l->entry_count &&
            sigil_saturn_extract(&src, l->entries[0].first_block, &bup, &bup_len) == SIGIL_OK &&
            sigil_saturn_volume_format_cart(&big, size, &raw) == SIGIL_OK) {
            if (sigil_saturn_inject(&big, bup, bup_len) != SIGIL_OK || sigil_saturn_volume_write(&big, &out, len) != SIGIL_OK) out = NULL;
            sigil_saturn_volume_free(&big);
        }
        free(bup);
        sigil_card_listing_free(l);
        sigil_saturn_volume_free(&src);
    }
    if (io) sigil_io_close(io);
    free(cart);
    return out;
}

/* Kronos keeps its volumes under kronos/saturn, the cart named by its size,
 * and with kronos_use_beetle_saves the Beetle names. Each layout collects
 * both volumes and restores them where the core reads them. */
static void check_kronos(void) {
    size_t int_len = 0;
    uint8_t *internal = sample(&g_saturn, "saturn", "rayman-bkr-bcr", "Rayman (USA) (R2)-internal.bkr", &int_len);
    static const struct { const char *option_key; const char *option_value; const char *int_path; const char *cart_path; size_t cart_size; } CASES[] = {
        { NULL, NULL, "kronos/saturn/" RAYMAN ".ram", "kronos/saturn/" RAYMAN "-ext512K.ram", 512u * 1024u },
        { "kronos_addon_cartridge", "1M_backup_ram", "kronos/saturn/" RAYMAN ".ram", "kronos/saturn/" RAYMAN "-ext1M.ram", 1024u * 1024u },
        { "kronos_addon_cartridge", "4M_backup_ram", "kronos/saturn/" RAYMAN ".ram", "kronos/saturn/" RAYMAN "-ext4M.ram", 4096u * 1024u },
        { "kronos_use_beetle_saves", "enabled", RAYMAN ".bkr", RAYMAN ".bcr", 512u * 1024u },
    };
    for (size_t i = 0; internal && i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        size_t cart_len = 0;
        uint8_t *cart = rayman_cart_of(CASES[i].cart_size, &cart_len);
        if (!cart) { fail("kronos", "setup failed"); continue; }
        mem_root root = {0};
        root_put(&root, CASES[i].int_path, internal, int_len);
        root_put(&root, CASES[i].cart_path, cart, cart_len);
        game g;
        make_game(&g, &root, "kronos", "saturn", RAYMAN ".cue", SIGIL_SYNC_MANAGED);
        if (CASES[i].option_key) add_option(&g, CASES[i].option_key, CASES[i].option_value);
        sigil_sync_result *r = NULL;
        if (sigil_collect(&g.req, &r) != SIGIL_OK || r->shape != SIGIL_SAVE_SHAPE_MULTI || r->holding) {
            fail("kronos", CASES[i].cart_path);
        } else {
            mem_root empty = {0};
            game fresh;
            make_game(&fresh, &empty, "kronos", "saturn", RAYMAN ".cue", SIGIL_SYNC_MANAGED);
            if (CASES[i].option_key) add_option(&fresh, CASES[i].option_key, CASES[i].option_value);
            sigil_sync_result *restored = NULL, *back = NULL;
            if (sigil_restore(&fresh.req, r->data, r->len, &restored) != SIGIL_OK) {
                fail("kronos", "restore into an empty root failed");
            } else {
                mem_file *ic = root_find(&empty, CASES[i].int_path), *cc = root_find(&empty, CASES[i].cart_path);
                if (!ic || !cc || ic->len != 32768 || cc->len != CASES[i].cart_size) fail("kronos", "the volumes didn't go where Kronos reads them");
                refresh(&fresh, &empty);
                use_state(&fresh, restored);
                if (sigil_collect(&fresh.req, &back) != SIGIL_OK || strcmp(back->identity_hash, r->identity_hash) != 0) {
                    fail("kronos", "saves differ after the trip");
                }
            }
            sigil_sync_result_free(back);
            sigil_sync_result_free(restored);
            root_free(&empty);
        }
        sigil_sync_result_free(r);
        root_free(&root);
        free(cart);
    }
    free(internal);
}

/* ---- Saturn shared volumes ------------------------------------------------------ */

#define SHARED_BKR "mednafen_saturn_libretro_shared.bkr"
#define ZWEI_CUE   "Panzer Dragoon II Zwei (USA).cue"
#define DWARF_CUE  "Three Dirty Dwarves (USA).cue"

static const char *const ZWEI_IDS[] = { "MK-81022" };
static const char *const DWARF_IDS[] = { "T-30401H" };
static const char *const ZWEI_AND_DWARF_IDS[] = { "MK-81022", "T-30401H" };

/* A raw 32 KiB Saturn internal volume holding `n` .BUP saves. */
static uint8_t *saturn_volume(const uint8_t *const *bups, const size_t *lens, size_t n, size_t *out_len) {
    sigil_saturn_volume v;
    sigil_bram_storage raw;
    memset(&raw, 0, sizeof(raw));
    raw.filler = -1;
    uint8_t *out = NULL;
    if (sigil_saturn_volume_format(&v, SATURN_INTERNAL_SIZE, &raw) != SIGIL_OK) return NULL;
    bool ok = true;
    for (size_t i = 0; i < n && ok; i++) ok = sigil_saturn_inject(&v, bups[i], lens[i]) == SIGIL_OK;
    if (!ok || sigil_saturn_volume_write(&v, &out, out_len) != SIGIL_OK) out = NULL;
    sigil_saturn_volume_free(&v);
    return out;
}

/* `bup` with one byte of its data flipped: a newer save of the same name. */
static uint8_t *bup_variant(const uint8_t *bup, size_t len, size_t at) {
    uint8_t *out = (uint8_t *)malloc(len);
    if (!out) return NULL;
    memcpy(out, bup, len);
    out[SATURN_BUP_HEADER_SIZE + at] ^= 0xFF;
    return out;
}

/* A Beetle Saturn game on the shared internal volume. */
static void make_shared(game *g, mem_root *root, const char *content, const char *const *ids, size_t id_count, int mode) {
    make_game(g, root, "mednafen_saturn", "saturn", content, mode);
    add_option(g, "beetle_saturn_save_method", "mednafen");
    add_option(g, "beetle_saturn_shared_int", "enabled");
    g->req.game_ids = ids;
    g->req.game_id_count = id_count;
}

/* The saves of the shared-volume checks: Zwei and the Dwarves have table
 * rows, Tokimeki has none. */
typedef struct {
    uint8_t *zwei, *dwarf, *toki;
    size_t   zwei_len, dwarf_len, toki_len;
} shared_saves;

static bool load_shared_saves(shared_saves *s) {
    memset(s, 0, sizeof(*s));
    s->zwei = sample(&g_saturn, "saturn", "pandra-zwei-bup", "PANDRA_ZWEI.BUP", &s->zwei_len);
    s->dwarf = sample(&g_saturn, "saturn", "three-dirty-dwarves-bup", NULL, &s->dwarf_len);
    s->toki = sample(&g_saturn, "saturn", "tokimeki-bup", "TOKIMEKI_99.BUP", &s->toki_len);
    return s->zwei && s->dwarf && s->toki;
}

static void free_shared_saves(shared_saves *s) {
    free(s->zwei);
    free(s->dwarf);
    free(s->toki);
}

/* A unit of one internal volume holding `bup`. */
static uint8_t *unit_of(const uint8_t *bup, size_t len, size_t *out_len) {
    const uint8_t *one[1] = { bup };
    return saturn_volume(one, &len, 1, out_len);
}

/* The identity collect gives Zwei's saves on a per-game volume holding `bup`. */
static bool zwei_identity(const uint8_t *bup, size_t len, char out[33]) {
    size_t vol_len = 0;
    uint8_t *vol = unit_of(bup, len, &vol_len);
    mem_root root = {0};
    if (vol) root_put(&root, "Panzer Dragoon II Zwei (USA).srm", vol, vol_len);
    game g;
    make_game(&g, &root, "mednafen_saturn", "saturn", ZWEI_CUE, SIGIL_SYNC_MANAGED);
    sigil_sync_result *r = NULL;
    bool ok = vol && sigil_collect(&g.req, &r) == SIGIL_OK && r->data;
    if (ok) snprintf(out, 33, "%s", r->identity_hash);
    sigil_sync_result_free(r);
    root_free(&root);
    free(vol);
    return ok;
}

/* Unmanaged on a shared volume: restore injects only when the volume is as
 * the last collect or restore saw it, so a second restore needs no collect
 * between; a collect after the core overwrote the restore asks for it again,
 * while local saves that match neither side come back as a change, with no
 * flag picking a winner. */
static void check_saturn_unmanaged(void) {
    shared_saves s;
    if (!load_shared_saves(&s)) { free_shared_saves(&s); fail("saturn unmanaged", "setup failed"); return; }
    uint8_t *z1 = bup_variant(s.zwei, s.zwei_len, 1), *z2 = bup_variant(s.zwei, s.zwei_len, 2);
    uint8_t *z3 = bup_variant(s.zwei, s.zwei_len, 3), *toki2 = bup_variant(s.toki, s.toki_len, 1);
    const uint8_t *v0b[3] = { s.zwei, s.dwarf, s.toki }, *v0t[3] = { s.zwei, s.dwarf, toki2 };
    const uint8_t *v1b[3] = { z1, s.dwarf, s.toki }, *v3b[3] = { z3, s.dwarf, s.toki };
    size_t lens[3] = { s.zwei_len, s.dwarf_len, s.toki_len };
    size_t v0_len = 0, v0t_len = 0, v1_len = 0, v3_len = 0, u1_len = 0, u2_len = 0;
    uint8_t *v0 = saturn_volume(v0b, lens, 3, &v0_len), *v0_toki = saturn_volume(v0t, lens, 3, &v0t_len);
    uint8_t *v1 = saturn_volume(v1b, lens, 3, &v1_len), *v3 = saturn_volume(v3b, lens, 3, &v3_len);
    uint8_t *u1 = unit_of(z1, s.zwei_len, &u1_len), *u2 = unit_of(z2, s.zwei_len, &u2_len);
    char id3[33] = "";
    mem_root root = {0};
    sigil_sync_result *seen = NULL, *r = NULL, *again = NULL, *after = NULL;
    if (!v0 || !v0_toki || !v1 || !v3 || !u1 || !u2 || !zwei_identity(z3, s.zwei_len, id3)) {
        fail("saturn unmanaged", "setup failed");
        goto done;
    }
    root_put(&root, SHARED_BKR, v0, v0_len);
    game g;
    make_shared(&g, &root, ZWEI_CUE, ZWEI_IDS, 1, SIGIL_SYNC_UNMANAGED);
    if (sigil_collect(&g.req, &seen) != SIGIL_OK || !seen->data) { fail("saturn unmanaged", "collect failed"); goto done; }
    use_state(&g, seen);

    root_put(&root, SHARED_BKR, v0_toki, v0t_len);
    root.writes = 0;
    if (sigil_restore(&g.req, u1, u1_len, &r) != SIGIL_ERR_UNCOLLECTED || root.writes != 0) {
        fail("saturn unmanaged", "a volume changed since the collect took an inject");
    }
    sigil_sync_result_free(r);
    r = NULL;

    root_put(&root, SHARED_BKR, v0, v0_len);
    if (sigil_restore(&g.req, u1, u1_len, &r) != SIGIL_OK) { fail("saturn unmanaged", "restore failed"); goto done; }
    use_state(&g, r);
    if (sigil_restore(&g.req, u2, u2_len, &again) != SIGIL_OK) {
        fail("saturn unmanaged", "a second restore refused the volume the first one left");
        goto done;
    }
    use_state(&g, again);

    root_put(&root, SHARED_BKR, v1, v1_len);
    if (sigil_collect(&g.req, &after) != SIGIL_OK || !after->restore_again || after->changed) {
        fail("saturn unmanaged", "a core that wrote back the replaced save didn't ask for the restore again");
    }
    sigil_sync_result_free(after);
    after = NULL;
    root_put(&root, SHARED_BKR, v3, v3_len);
    if (sigil_collect(&g.req, &after) != SIGIL_OK || after->restore_again || !after->changed ||
        strcmp(after->identity_hash, id3) != 0) {
        fail("saturn unmanaged", "local saves matching neither side didn't come back as a plain change");
    }
done:
    sigil_sync_result_free(after);
    sigil_sync_result_free(again);
    sigil_sync_result_free(r);
    sigil_sync_result_free(seen);
    root_free(&root);
    free(u1); free(u2); free(v0); free(v0_toki); free(v1); free(v3);
    free(z1); free(z2); free(z3); free(toki2);
    free_shared_saves(&s);
}

/* Managed: a swap refuses while a save with no owner changed after its
 * holding unit went up, so it can't be swapped away unsaved. A managed
 * swap's `prepared` doesn't make an unmanaged collect take others' saves. */
/* A listed volume the client can't open is an I/O error, never an absent
 * volume: restore would otherwise write a fresh one over the other games'
 * saves on it. */
static void check_unopenable_volume(void) {
    shared_saves s;
    if (!load_shared_saves(&s)) { free_shared_saves(&s); fail("unreadable volume", "setup failed"); return; }
    const uint8_t *both[2] = { s.zwei, s.toki };
    size_t lens[2] = { s.zwei_len, s.toki_len };
    size_t v_len = 0, u_len = 0;
    uint8_t *v = saturn_volume(both, lens, 2, &v_len), *u = unit_of(s.zwei, s.zwei_len, &u_len);
    for (int mode = SIGIL_SYNC_MANAGED; v && u && mode <= SIGIL_SYNC_UNMANAGED; mode++) {
        mem_root root = {0};
        root_put(&root, SHARED_BKR, v, v_len);
        snprintf(root.unreadable, sizeof(root.unreadable), "%s", SHARED_BKR);
        game g;
        make_shared(&g, &root, ZWEI_CUE, ZWEI_IDS, 1, mode);
        sigil_sync_result *r = NULL;
        if (sigil_collect(&g.req, &r) != SIGIL_ERR_IO) fail("unreadable volume", "collect read an unopenable volume as empty");
        sigil_sync_result_free(r);
        g.req.overwrite_local = 1;
        r = NULL;
        if (sigil_restore(&g.req, u, u_len, &r) != SIGIL_ERR_IO) fail("unreadable volume", "restore went ahead");
        if (root.writes) fail("unreadable volume", "restore wrote over a volume it couldn't read");
        sigil_sync_result_free(r);
        root_free(&root);
    }
    if (!v || !u) fail("unreadable volume", "setup failed");
    free(v);
    free(u);
    free_shared_saves(&s);
}

static void check_saturn_swap_guard(void) {
    shared_saves s;
    if (!load_shared_saves(&s)) { free_shared_saves(&s); fail("saturn swap guard", "setup failed"); return; }
    uint8_t *toki2 = bup_variant(s.toki, s.toki_len, 1), *z1 = bup_variant(s.zwei, s.zwei_len, 1);
    const uint8_t *vb[2] = { s.zwei, s.toki }, *vt[2] = { s.zwei, toki2 };
    size_t lens[2] = { s.zwei_len, s.toki_len };
    size_t v_len = 0, vt_len = 0, u1_len = 0;
    uint8_t *v = saturn_volume(vb, lens, 2, &v_len), *v_toki = saturn_volume(vt, lens, 2, &vt_len);
    uint8_t *u1 = unit_of(z1, s.zwei_len, &u1_len);
    mem_root root = {0};
    sigil_sync_result *held = NULL, *r = NULL, *loose = NULL;
    if (!v || !v_toki || !u1) { fail("saturn swap guard", "setup failed"); goto done; }
    root_put(&root, SHARED_BKR, v, v_len);
    game g;
    make_shared(&g, &root, ZWEI_CUE, ZWEI_IDS, 1, SIGIL_SYNC_MANAGED);
    if (sigil_collect(&g.req, &held) != SIGIL_OK || !held->holding) { fail("saturn swap guard", "collect held nothing"); goto done; }
    use_state(&g, held);
    root_put(&root, SHARED_BKR, v_toki, vt_len);
    root.writes = 0;
    if (sigil_restore(&g.req, u1, u1_len, &r) != SIGIL_ERR_UNCOLLECTED || root.writes != 0) {
        fail("saturn swap guard", "a save changed after its holding unit went up was swapped away");
    }
    sigil_sync_result_free(r);
    r = NULL;
    root_put(&root, SHARED_BKR, v, v_len);
    if (sigil_restore(&g.req, u1, u1_len, &r) != SIGIL_OK) { fail("saturn swap guard", "swap of a held volume failed"); goto done; }

    root_put(&root, SHARED_BKR, v, v_len);
    game u;
    make_shared(&u, &root, ZWEI_CUE, ZWEI_IDS, 1, SIGIL_SYNC_UNMANAGED);
    use_state(&u, r);
    if (sigil_collect(&u.req, &loose) != SIGIL_OK || loose->unowned_count != 1 || strcmp(loose->unowned[0], "TOKIMEKI_99") != 0) {
        fail("saturn swap guard", "a managed swap's prepared record gave an unmanaged collect another game's save");
    }
done:
    sigil_sync_result_free(loose);
    sigil_sync_result_free(r);
    sigil_sync_result_free(held);
    root_free(&root);
    free(v); free(v_toki); free(u1); free(toki2); free(z1);
    free_shared_saves(&s);
}

/* `vol` with the save named `name` broken: its archive claims more data than
 * its block list holds, so it lists as corrupt under its name. */
static uint8_t *saturn_broken(const uint8_t *vol, size_t len, const char *name) {
    sigil_card_listing *l = listing_of(vol, len);
    uint8_t *out = l ? (uint8_t *)malloc(len) : NULL;
    bool broken = false;
    if (out) {
        memcpy(out, vol, len);
        for (size_t i = 0; i < l->entry_count; i++) {
            if (strcmp(l->entries[i].name, name) != 0) continue;
            sigil_write_be32(out + (size_t)l->entries[i].first_block * 64 + 0x1E, 0x00FFFFFFu);
            broken = true;
        }
    }
    sigil_card_listing_free(l);
    if (!broken) { free(out); return NULL; }
    return out;
}

/* A volume naming a corrupt save that may be the game's is DAMAGED naming
 * the volume on collect and on restore in both modes, writing nothing:
 * reading on would take that save as deleted. A shared volume where the
 * corrupt save is another game's syncs unmanaged, keeping it, but refuses a
 * managed swap, which would drop it. */
static void check_corrupt_saves(const sigil_sync_result *lunar) {
    shared_saves s;
    size_t lunar_len = 0;
    uint8_t *lunar_vol = segacd("lunar-ecc-brm", &lunar_len);
    uint8_t *lunar_dir = lunar_vol ? (uint8_t *)malloc(lunar_len) : NULL;
    if (!load_shared_saves(&s) || !lunar_dir || !lunar) { fail("corrupt saves", "setup failed"); goto out; }
    memcpy(lunar_dir, lunar_vol, lunar_len);
    lunar_vol[0x0048] ^= 0xC0;
    lunar_dir[0x1F88] ^= 0xC0;

    size_t zwei_len = 0, unit_len = 0, both_len = 0, toki_len = 0;
    uint8_t *zwei = unit_of(s.zwei, s.zwei_len, &unit_len);
    uint8_t *zwei_broken = zwei ? saturn_broken(zwei, unit_len, "PANDRA_ZWEI") : NULL;
    zwei_len = unit_len;
    const uint8_t *vb[2] = { s.zwei, s.dwarf };
    size_t lens[2] = { s.zwei_len, s.dwarf_len };
    uint8_t *both = saturn_volume(vb, lens, 2, &both_len);
    uint8_t *both_broken = both ? saturn_broken(both, both_len, "PANDRA_ZWEI") : NULL;
    const uint8_t *tb[2] = { s.zwei, s.toki };
    size_t tlens[2] = { s.zwei_len, s.toki_len };
    uint8_t *with_toki = saturn_volume(tb, tlens, 2, &toki_len);
    uint8_t *toki_broken = with_toki ? saturn_broken(with_toki, toki_len, "TOKIMEKI_99") : NULL;
    if (!zwei_broken || !both_broken || !toki_broken) { fail("corrupt saves", "setup failed"); goto saturn_out; }

    struct {
        const char *what, *path, *layout, *platform, *content, *opt_key, *opt_value;
        const uint8_t *vol;
        size_t len;
        const uint8_t *unit;
        size_t unit_len;
        bool shared;
    } CASES[] = {
        { "sega cd per-game", LUNAR_BRM, "genesis_plus_gx", "segacd", LUNAR, "genesis_plus_gx_system_bram", "per game",
          lunar_vol, lunar_len, lunar->data, lunar->len, false },
        { "sega cd shared, a save it no longer names", "scd_U.brm", "genesis_plus_gx", "segacd", LUNAR, NULL, NULL,
          lunar_dir, lunar_len, lunar->data, lunar->len, false },
        { "saturn per-game", "Panzer Dragoon II Zwei (USA).srm", "mednafen_saturn", "saturn", ZWEI_CUE, NULL, NULL,
          zwei_broken, zwei_len, zwei, unit_len, false },
        { "saturn shared", SHARED_BKR, NULL, NULL, ZWEI_CUE, NULL, NULL, both_broken, both_len, zwei, unit_len, true },
    };
    for (size_t c = 0; c < sizeof(CASES) / sizeof(CASES[0]); c++) {
        for (int mode = SIGIL_SYNC_MANAGED; mode <= SIGIL_SYNC_UNMANAGED; mode++) {
            mem_root root = {0};
            root_put(&root, CASES[c].path, CASES[c].vol, CASES[c].len);
            game g;
            if (CASES[c].shared) {
                make_shared(&g, &root, ZWEI_CUE, ZWEI_IDS, 1, mode);
            } else {
                make_game(&g, &root, CASES[c].layout, CASES[c].platform, CASES[c].content, mode);
                if (CASES[c].opt_key) add_option(&g, CASES[c].opt_key, CASES[c].opt_value);
            }
            g.req.overwrite_local = 1;
            sigil_sync_result *seen = NULL, *r = NULL;
            if (sigil_collect(&g.req, &seen) != SIGIL_ERR_DAMAGED || !seen || strcmp(seen->problem, CASES[c].path) != 0) {
                fail("corrupt saves", CASES[c].what);
            }
            if (sigil_restore(&g.req, CASES[c].unit, CASES[c].unit_len, &r) != SIGIL_ERR_DAMAGED || !r ||
                strcmp(r->problem, CASES[c].path) != 0 || root.writes != 0) {
                fail("corrupt saves", CASES[c].what);
            }
            sigil_sync_result_free(r);
            sigil_sync_result_free(seen);
            root_free(&root);
        }
    }

    for (int mode = SIGIL_SYNC_MANAGED; mode <= SIGIL_SYNC_UNMANAGED; mode++) {
        mem_root root = {0};
        root_put(&root, SHARED_BKR, toki_broken, toki_len);
        game g;
        make_shared(&g, &root, ZWEI_CUE, ZWEI_IDS, 1, mode);
        sigil_sync_result *seen = NULL, *r = NULL;
        if (sigil_collect(&g.req, &seen) != SIGIL_OK || !seen->data) {
            fail("corrupt saves", "another game's corrupt save stopped a collect");
        } else {
            use_state(&g, seen);
            g.req.overwrite_local = 1;
            int rc = sigil_restore(&g.req, zwei, unit_len, &r);
            mem_file *f = root_find(&root, SHARED_BKR);
            sigil_card_listing *after = f ? listing_of(f->data, f->len) : NULL;
            if (mode == SIGIL_SYNC_MANAGED &&
                (rc != SIGIL_ERR_DAMAGED || !r || strcmp(r->problem, SHARED_BKR) != 0 || root.writes != 0)) {
                fail("corrupt saves", "a managed swap would drop another game's corrupt save");
            }
            if (mode == SIGIL_SYNC_UNMANAGED && (rc != SIGIL_OK || !after || after->corrupt_entry_count != 1 ||
                                                 strcmp(after->corrupt_entries[0].name, "TOKIMEKI_99") != 0)) {
                fail("corrupt saves", "an unmanaged restore didn't keep another game's corrupt save");
            }
            sigil_card_listing_free(after);
        }
        sigil_sync_result_free(r);
        sigil_sync_result_free(seen);
        root_free(&root);
    }

saturn_out:
    free(zwei); free(zwei_broken); free(both); free(both_broken); free(with_toki); free(toki_broken);
out:
    free(lunar_vol);
    free(lunar_dir);
    free_shared_saves(&s);
}

/* Who owns a save on a shared volume, each rule against the next: the
 * user's claim beats a learned owner; a learned owner beats the name table
 * and the managed swap record; on a per-game volume only a companion's
 * learned owner counts. */
static void check_owner_precedence(void) {
    shared_saves s;
    if (!load_shared_saves(&s)) { free_shared_saves(&s); fail("owner precedence", "setup failed"); return; }
    const uint8_t *vb[2] = { s.zwei, s.dwarf };
    size_t lens[2] = { s.zwei_len, s.dwarf_len };
    size_t v_len = 0, du_len = 0;
    uint8_t *v = saturn_volume(vb, lens, 2, &v_len), *dwarf_unit = unit_of(s.dwarf, s.dwarf_len, &du_len);
    mem_root root = {0};
    sigil_sync_result *learned = NULL, *r = NULL, *swapped = NULL;
    if (!v || !dwarf_unit) { fail("owner precedence", "setup failed"); goto done; }
    root_put(&root, SHARED_BKR, v, v_len);
    game d;
    make_shared(&d, &root, DWARF_CUE, DWARF_IDS, 1, SIGIL_SYNC_MANAGED);
    if (sigil_collect(&d.req, &learned) != SIGIL_OK || !learned->data) { fail("owner precedence", "the Dwarves' collect failed"); goto done; }

    static const char *const CLAIM[] = { "THREE_DIRTY" };
    struct { const char *what; const char *const *ids; size_t id_count; const char *const *claimed; bool takes; } CASES[] = {
        { "a claim lost to a learned owner", ZWEI_IDS, 1, CLAIM, true },
        { "the name table beat a learned owner", ZWEI_AND_DWARF_IDS, 2, NULL, false },
    };
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        game g;
        make_shared(&g, &root, ZWEI_CUE, CASES[i].ids, CASES[i].id_count, SIGIL_SYNC_MANAGED);
        use_state(&g, learned);
        g.req.claimed = CASES[i].claimed;
        g.req.claimed_count = CASES[i].claimed ? 1 : 0;
        r = NULL;
        sigil_card_listing *l = NULL;
        if (sigil_collect(&g.req, &r) != SIGIL_OK || !r->data) {
            fail("owner precedence", "collect failed");
        } else {
            l = listing_of(r->data, r->len);
            if (has_name(l, "THREE_DIRTY") != CASES[i].takes) fail("owner precedence", CASES[i].what);
        }
        sigil_card_listing_free(l);
        sigil_sync_result_free(r);
    }

    game z;
    make_shared(&z, &root, ZWEI_CUE, ZWEI_IDS, 1, SIGIL_SYNC_MANAGED);
    use_state(&z, learned);
    size_t zu_len = 0;
    uint8_t *zwei_unit = unit_of(s.zwei, s.zwei_len, &zu_len);
    if (!zwei_unit || sigil_restore(&z.req, zwei_unit, zu_len, &swapped) != SIGIL_OK) {
        fail("owner precedence", "the managed swap failed");
    } else {
        root_put(&root, SHARED_BKR, v, v_len);
        refresh(&z, &root);
        use_state(&z, swapped);
        r = NULL;
        sigil_card_listing *l = NULL;
        if (sigil_collect(&z.req, &r) != SIGIL_OK || !r->data || has_name(l = listing_of(r->data, r->len), "THREE_DIRTY")) {
            fail("owner precedence", "the swap record beat a learned owner");
        }
        sigil_card_listing_free(l);
        sigil_sync_result_free(r);
    }
    free(zwei_unit);

    mem_root own = {0};
    root_put(&own, "Panzer Dragoon II Zwei (USA).srm", v, v_len);
    game p;
    make_game(&p, &own, "mednafen_saturn", "saturn", ZWEI_CUE, SIGIL_SYNC_MANAGED);
    p.req.game_ids = ZWEI_IDS;
    p.req.game_id_count = 1;
    sigil_sync_companion dwarves = { DWARF_IDS, 1, dwarf_unit, du_len };
    p.req.companions = &dwarves;
    p.req.companion_count = 1;
    p.req.overwrite_local = 1;
    size_t zu2_len = 0;
    uint8_t *zwei_unit2 = unit_of(s.zwei, s.zwei_len, &zu2_len);
    r = NULL;
    sigil_sync_result *back = NULL;
    if (!zwei_unit2 || sigil_restore(&p.req, zwei_unit2, zu2_len, &r) != SIGIL_OK) {
        fail("owner precedence", "restore with the companion failed");
    } else {
        refresh(&p, &own);
        use_state(&p, r);
        p.req.companions = NULL;
        p.req.companion_count = 0;
        sigil_card_listing *l = NULL;
        if (sigil_collect(&p.req, &back) != SIGIL_OK || !back->data || !has_name(l = listing_of(back->data, back->len), "THREE_DIRTY")) {
            fail("owner precedence", "a former companion's learned owner took a save off the game's own volume");
        }
        sigil_card_listing_free(l);
    }
    free(zwei_unit2);
    sigil_sync_result_free(back);
    sigil_sync_result_free(r);
    root_free(&own);
done:
    sigil_sync_result_free(swapped);
    sigil_sync_result_free(learned);
    root_free(&root);
    free(v);
    free(dwarf_unit);
    free_shared_saves(&s);
}

/* Managed, a companion whose saves sit on a shared volume keeps them across
 * a swap that carries no unit for it. */
static void check_saturn_companion_shared(void) {
    shared_saves s;
    if (!load_shared_saves(&s)) { free_shared_saves(&s); fail("saturn companion shared", "setup failed"); return; }
    uint8_t *z1 = bup_variant(s.zwei, s.zwei_len, 1);
    size_t v_len = 0, zu_len = 0, z1u_len = 0, du_len = 0;
    uint8_t *v = unit_of(s.zwei, s.zwei_len, &v_len);
    uint8_t *zwei_unit = unit_of(s.zwei, s.zwei_len, &zu_len), *z1_unit = z1 ? unit_of(z1, s.zwei_len, &z1u_len) : NULL;
    uint8_t *dwarf_unit = unit_of(s.dwarf, s.dwarf_len, &du_len);
    mem_root root = {0};
    sigil_sync_result *first = NULL, *r = NULL, *again = NULL;
    if (!v || !zwei_unit || !z1_unit || !dwarf_unit) { fail("saturn companion shared", "setup failed"); goto done; }
    root_put(&root, SHARED_BKR, v, v_len);
    game g;
    make_shared(&g, &root, ZWEI_CUE, ZWEI_IDS, 1, SIGIL_SYNC_MANAGED);
    sigil_sync_companion dwarves = { DWARF_IDS, 1, dwarf_unit, du_len };
    g.req.companions = &dwarves;
    g.req.companion_count = 1;
    if (sigil_collect(&g.req, &first) != SIGIL_OK) { fail("saturn companion shared", "collect failed"); goto done; }
    use_state(&g, first);
    if (sigil_restore(&g.req, zwei_unit, zu_len, &r) != SIGIL_OK) { fail("saturn companion shared", "restore with the companion's unit failed"); goto done; }
    refresh(&g, &root);
    use_state(&g, r);
    dwarves.unit = NULL;
    dwarves.unit_len = 0;
    mem_file *f = NULL;
    sigil_card_listing *l = NULL;
    if (sigil_restore(&g.req, z1_unit, z1u_len, &again) != SIGIL_OK || !(f = root_find(&root, SHARED_BKR)) ||
        !has_name(l = listing_of(f->data, f->len), "THREE_DIRTY")) {
        fail("saturn companion shared", "a swap without the companion's unit dropped its save");
    }
    sigil_card_listing_free(l);
done:
    sigil_sync_result_free(again);
    sigil_sync_result_free(r);
    sigil_sync_result_free(first);
    root_free(&root);
    free(v); free(zwei_unit); free(z1_unit); free(dwarf_unit); free(z1);
    free_shared_saves(&s);
}

/* `like` with its data replaced by `size` zero bytes. */
static uint8_t *sized_bup(const uint8_t *like, uint32_t size, size_t *len) {
    *len = SATURN_BUP_HEADER_SIZE + size;
    uint8_t *out = (uint8_t *)calloc(1, *len);
    if (!out) return NULL;
    memcpy(out, like, SATURN_BUP_HEADER_SIZE);
    sigil_write_be32(out + 0x2C, size);
    return out;
}

/* The blocks `bup` takes on a fresh volume of `size` bytes, as the listing
 * counts them; 0 when it doesn't go in. */
static uint32_t blocks_on_fresh(const uint8_t *bup, size_t len, size_t size, bool cart) {
    sigil_saturn_volume v;
    sigil_bram_storage raw;
    memset(&raw, 0, sizeof(raw));
    raw.filler = -1;
    int rc = cart ? sigil_saturn_volume_format_cart(&v, size, &raw) : sigil_saturn_volume_format(&v, size, &raw);
    if (rc != SIGIL_OK) return 0;
    uint32_t blocks = 0;
    sigil_card_listing *l = NULL;
    if (sigil_saturn_inject(&v, bup, len) == SIGIL_OK && sigil_saturn_list(&v, &l) == SIGIL_OK && l->entry_count == 1) {
        blocks = l->entries[0].blocks;
    }
    sigil_card_listing_free(l);
    sigil_saturn_volume_free(&v);
    return blocks;
}

static uint32_t free_on_fresh(size_t size, bool cart) {
    sigil_saturn_volume v;
    sigil_bram_storage raw;
    memset(&raw, 0, sizeof(raw));
    raw.filler = -1;
    int rc = cart ? sigil_saturn_volume_format_cart(&v, size, &raw) : sigil_saturn_volume_format(&v, size, &raw);
    if (rc != SIGIL_OK) return 0;
    uint32_t n = 0;
    sigil_card_listing *l = NULL;
    if (sigil_saturn_list(&v, &l) == SIGIL_OK) n = l->free_blocks;
    sigil_card_listing_free(l);
    sigil_saturn_volume_free(&v);
    return n;
}

/* Kronos's default 512 KiB cart refuses a cart save that needs more, with
 * the shortfall in the cart's 512-byte blocks. */
static void check_cart_too_small(void) {
    size_t int_len = 0, bup_len = 0, cart_len = 0;
    uint8_t *internal = sample(&g_saturn, "saturn", "rayman-bkr-bcr", "Rayman (USA) (R2)-internal.bkr", &int_len);
    shared_saves s;
    uint8_t *big = load_shared_saves(&s) ? sized_bup(s.zwei, 600u * 1024u, &bup_len) : NULL;
    uint8_t *cart = NULL;
    sigil_saturn_volume v;
    sigil_bram_storage raw;
    memset(&raw, 0, sizeof(raw));
    raw.filler = -1;
    if (big && sigil_saturn_volume_format_cart(&v, 1024u * 1024u, &raw) == SIGIL_OK) {
        if (sigil_saturn_inject(&v, big, bup_len) != SIGIL_OK || sigil_saturn_volume_write(&v, &cart, &cart_len) != SIGIL_OK) cart = NULL;
        sigil_saturn_volume_free(&v);
    }
    uint32_t need = big ? blocks_on_fresh(big, bup_len, 1024u * 1024u, true) : 0;
    uint32_t room = free_on_fresh(512u * 1024u, true);
    mem_root root = {0}, empty = {0};
    sigil_sync_result *unit = NULL, *r = NULL;
    if (!internal || !cart || !need || need <= room) { fail("cart too small", "setup failed"); goto done; }
    root_put(&root, "kronos/saturn/" RAYMAN ".ram", internal, int_len);
    root_put(&root, "kronos/saturn/" RAYMAN "-ext1M.ram", cart, cart_len);
    game g, fresh;
    make_game(&g, &root, "kronos", "saturn", RAYMAN ".cue", SIGIL_SYNC_MANAGED);
    add_option(&g, "kronos_addon_cartridge", "1M_backup_ram");
    if (sigil_collect(&g.req, &unit) != SIGIL_OK || !unit->data) { fail("cart too small", "setup collect failed"); goto done; }
    /* Kronos's default cart, and the 4 Mbit .bcr that Beetle (per game and
     * shared) and Kronos's Beetle-saves mode always use. */
    static const struct { const char *layout, *key, *value; } CARTS[] = {
        { "kronos", NULL, NULL },
        { "kronos", "kronos_use_beetle_saves", "enabled" },
        { "mednafen_saturn", NULL, NULL },
        { "mednafen_saturn", "beetle_saturn_shared_ext", "enabled" },
    };
    for (size_t i = 0; i < sizeof(CARTS) / sizeof(CARTS[0]); i++) {
        memset(&empty, 0, sizeof(empty));
        make_game(&fresh, &empty, CARTS[i].layout, "saturn", RAYMAN ".cue", SIGIL_SYNC_MANAGED);
        if (CARTS[i].key) add_option(&fresh, CARTS[i].key, CARTS[i].value);
        if (sigil_restore(&fresh.req, unit->data, unit->len, &r) != SIGIL_ERR_NO_SPACE || !r || empty.writes != 0 ||
            strcmp(r->problem, "PANDRA_ZWEI") != 0 || r->blocks_short != need - room) {
            char what[160];
            snprintf(what, sizeof(what), "%s %s: a cart save too big for the cart wasn't refused with its shortfall",
                     CARTS[i].layout, CARTS[i].key ? CARTS[i].key : "");
            fail("cart too small", what);
        }
        sigil_sync_result_free(r);
        r = NULL;
        root_free(&empty);
        memset(&empty, 0, sizeof(empty));
    }
done:
    sigil_sync_result_free(r);
    sigil_sync_result_free(unit);
    root_free(&root);
    root_free(&empty);
    free(cart);
    free(big);
    free(internal);
    free_shared_saves(&s);
}

/* Real saves from Kronos and Beetle Saturn (Sega Rally's records and ghosts
 * on the 512 KiB cart) move between the cores, Kronos's Beetle-compatible
 * saves included: what one setup's files collect to restores under another
 * and collects back to the same saves. */
static void check_kronos_beetle_cross(void) {
    static const char *const KRONOS_INT = "kronos/saturn/Sega Rally Championship (USA).ram";
    static const char *const KRONOS_CART = "kronos/saturn/Sega Rally Championship (USA)-ext512K.ram";
    static const char *const BEETLE_INT = "Sega Rally Championship (USA).bkr";
    static const char *const BEETLE_CART = "Sega Rally Championship (USA).bcr";
    /* Each side's option: Beetle's mednafen save method, or Kronos's
     * Beetle-compatible saves, which write Beetle's file names. */
    static const char *const BEETLE_KEY = "beetle_saturn_save_method", *const BEETLE_VALUE = "mednafen";
    static const char *const KRONOS_KEY = "kronos_use_beetle_saves", *const KRONOS_VALUE = "enabled";
    static const struct {
        const char *from, *from_key, *from_value, *to, *to_key, *to_value, *id, *int_path, *cart_path, *want_cart;
    } CROSS[] = {
        { "kronos", NULL, NULL, "mednafen_saturn", BEETLE_KEY, BEETLE_VALUE, "sega-rally-kronos", KRONOS_INT, KRONOS_CART,
          BEETLE_CART },
        { "mednafen_saturn", BEETLE_KEY, BEETLE_VALUE, "kronos", NULL, NULL, "sega-rally-beetle", BEETLE_INT, BEETLE_CART,
          KRONOS_CART },
        { "kronos", KRONOS_KEY, KRONOS_VALUE, "kronos", NULL, NULL, "sega-rally-kronos-beetle-saves", BEETLE_INT,
          BEETLE_CART, KRONOS_CART },
    };
    for (size_t i = 0; i < sizeof(CROSS) / sizeof(CROSS[0]); i++) {
        size_t int_len = 0, cart_len = 0;
        uint8_t *internal = sample(&g_saturn, "saturn", CROSS[i].id, CROSS[i].int_path, &int_len);
        uint8_t *cart = sample(&g_saturn, "saturn", CROSS[i].id, CROSS[i].cart_path, &cart_len);
        mem_root src = {0}, dst = {0};
        sigil_sync_result *unit = NULL, *r = NULL, *back = NULL;
        if (!internal || !cart) { fail("kronos beetle cross", "setup failed"); goto next; }
        root_put(&src, CROSS[i].int_path, internal, int_len);
        root_put(&src, CROSS[i].cart_path, cart, cart_len);
        game from, to;
        make_game(&from, &src, CROSS[i].from, "saturn", "Sega Rally Championship (USA).cue", SIGIL_SYNC_MANAGED);
        if (CROSS[i].from_key) add_option(&from, CROSS[i].from_key, CROSS[i].from_value);
        if (sigil_collect(&from.req, &unit) != SIGIL_OK || !unit->data || unit->shape != SIGIL_SAVE_SHAPE_MULTI) {
            fail("kronos beetle cross", "collect from the real files failed");
            goto next;
        }
        make_game(&to, &dst, CROSS[i].to, "saturn", "Sega Rally Championship (USA).cue", SIGIL_SYNC_MANAGED);
        if (CROSS[i].to_key) add_option(&to, CROSS[i].to_key, CROSS[i].to_value);
        const char *want_cart = CROSS[i].want_cart;
        mem_file *written = NULL;
        sigil_card_listing *l = NULL;
        if (sigil_restore(&to.req, unit->data, unit->len, &r) != SIGIL_OK || !(written = root_find(&dst, want_cart)) ||
            written->len != 512u * 1024u || !(l = listing_of(written->data, written->len)) ||
            !has_name(l, "SEGARALLY_0") || !has_name(l, "SEGARALLY_1")) {
            fail("kronos beetle cross", "the cart didn't restore where the other core reads it, as a 512 KiB cart");
        } else {
            refresh(&to, &dst);
            use_state(&to, r);
            if (sigil_collect(&to.req, &back) != SIGIL_OK || strcmp(back->identity_hash, unit->identity_hash) != 0) {
                fail("kronos beetle cross", "the saves differ after moving between the cores");
            }
        }
        sigil_card_listing_free(l);
    next:
        sigil_sync_result_free(back);
        sigil_sync_result_free(r);
        sigil_sync_result_free(unit);
        root_free(&src);
        root_free(&dst);
        free(internal);
        free(cart);
    }
}

/* A volume file that is empty is no volume yet; one holding a cart where
 * internal RAM goes is damaged. */
static void check_volume_files(void) {
    size_t cart_len = 0;
    uint8_t *cart = rayman_cart_of(512u * 1024u, &cart_len);
    shared_saves s;
    size_t unit_len = 0;
    uint8_t *unit = load_shared_saves(&s) ? unit_of(s.zwei, s.zwei_len, &unit_len) : NULL;
    if (!cart || !unit) { fail("volume files", "setup failed"); goto done; }
    for (int which = 0; which < 2; which++) {
        mem_root root = {0};
        root_put(&root, "Panzer Dragoon II Zwei (USA).srm", cart, which ? cart_len : 0);
        game g;
        make_game(&g, &root, "mednafen_saturn", "saturn", ZWEI_CUE, SIGIL_SYNC_MANAGED);
        sigil_sync_result *seen = NULL, *r = NULL;
        int collected = sigil_collect(&g.req, &seen);
        int restored = sigil_restore(&g.req, unit, unit_len, &r);
        if (which == 0 && (collected != SIGIL_OK || seen->data || restored != SIGIL_OK || root.writes != 1)) {
            fail("volume files", "an empty volume file wasn't taken as no volume");
        }
        if (which == 1 && (collected != SIGIL_ERR_DAMAGED || strcmp(seen->problem, "Panzer Dragoon II Zwei (USA).srm") != 0 ||
                           restored != SIGIL_ERR_DAMAGED || root.writes != 0)) {
            fail("volume files", "a cart where internal RAM goes wasn't damaged");
        }
        sigil_sync_result_free(r);
        sigil_sync_result_free(seen);
        root_free(&root);
    }
done:
    free(cart);
    free(unit);
    free_shared_saves(&s);
}

/* Managed, a kept companion's save that no longer fits beside the game's
 * new save refuses the swap naming the companion's save and its shortfall. */
static void check_kept_companion_overflow(void) {
    shared_saves s;
    if (!load_shared_saves(&s)) { free_shared_saves(&s); fail("kept companion overflow", "setup failed"); return; }
    uint32_t dwarf = blocks_on_fresh(s.dwarf, s.dwarf_len, SATURN_INTERNAL_SIZE, false);
    uint32_t room = free_on_fresh(SATURN_INTERNAL_SIZE, false);
    uint8_t *big = NULL;
    size_t big_len = 0;
    for (uint32_t size = 1024; dwarf && size < SATURN_INTERNAL_SIZE && !big; size += 8) {
        uint8_t *b = sized_bup(s.zwei, size, &big_len);
        if (b && blocks_on_fresh(b, big_len, SATURN_INTERNAL_SIZE, false) == room - dwarf + 1) big = b;
        else free(b);
    }
    size_t zu_len = 0, bu_len = 0, du_len = 0, v_len = 0;
    uint8_t *v = unit_of(s.zwei, s.zwei_len, &v_len);
    uint8_t *zwei_unit = unit_of(s.zwei, s.zwei_len, &zu_len), *dwarf_unit = unit_of(s.dwarf, s.dwarf_len, &du_len);
    uint8_t *big_unit = big ? unit_of(big, big_len, &bu_len) : NULL;
    mem_root root = {0};
    sigil_sync_result *first = NULL, *r = NULL, *again = NULL;
    if (!v || !zwei_unit || !dwarf_unit || !big_unit) { fail("kept companion overflow", "setup failed"); goto done; }
    root_put(&root, SHARED_BKR, v, v_len);
    game g;
    make_shared(&g, &root, ZWEI_CUE, ZWEI_IDS, 1, SIGIL_SYNC_MANAGED);
    sigil_sync_companion dwarves = { DWARF_IDS, 1, dwarf_unit, du_len };
    g.req.companions = &dwarves;
    g.req.companion_count = 1;
    if (sigil_collect(&g.req, &first) != SIGIL_OK) { fail("kept companion overflow", "collect failed"); goto done; }
    use_state(&g, first);
    if (sigil_restore(&g.req, zwei_unit, zu_len, &r) != SIGIL_OK) { fail("kept companion overflow", "setup restore failed"); goto done; }
    refresh(&g, &root);
    use_state(&g, r);
    dwarves.unit = NULL;
    dwarves.unit_len = 0;
    int writes = root.writes;
    if (sigil_restore(&g.req, big_unit, bu_len, &again) != SIGIL_ERR_NO_SPACE || !again || root.writes != writes ||
        strcmp(again->problem, "THREE_DIRTY") != 0 || again->blocks_short != 1) {
        fail("kept companion overflow", "a kept companion's save that no longer fits wasn't named with its shortfall");
    }
done:
    sigil_sync_result_free(again);
    sigil_sync_result_free(r);
    sigil_sync_result_free(first);
    root_free(&root);
    free(v); free(zwei_unit); free(dwarf_unit); free(big_unit); free(big);
    free_shared_saves(&s);
}

/* A game with more saves than the standard 32 KiB volume holds, on Yaba
 * Sanshiro's 4 MiB volume, travels in a unit the source volume's size and
 * restores; a file that exists keeps its own form, not the layout's. */
/* Every Saturn core keeps 32 KiB of internal backup RAM, so a unit whose
 * internal saves need more (the 4 MiB fallback) is refused with the blocks
 * the save lacks, counted at the volume's 64-byte blocks, and nothing is
 * written. Kronos formats any other size on boot. */
static void check_too_big_for_internal(void) {
    size_t touge_len = 0;
    uint8_t *touge = sample(&g_saturn, "saturn", "touge-king-bup", NULL, &touge_len);
    sigil_saturn_volume big, small;
    sigil_bram_storage raw;
    memset(&raw, 0, sizeof(raw));
    uint8_t *unit = NULL;
    size_t unit_len = 0;
    uint32_t need = 0, room = 0;
    if (touge && sigil_saturn_volume_format(&big, 4u * 1024u * 1024u, &raw) == SIGIL_OK) {
        sigil_card_listing *l = NULL;
        if (sigil_saturn_inject(&big, touge, touge_len) == SIGIL_OK && sigil_saturn_list(&big, &l) == SIGIL_OK &&
            l->entry_count == 1 && sigil_saturn_volume_write(&big, &unit, &unit_len) == SIGIL_OK) {
            need = l->entries[0].blocks;
        }
        sigil_card_listing_free(l);
        sigil_saturn_volume_free(&big);
    }
    if (sigil_saturn_volume_format(&small, SATURN_INTERNAL_SIZE, &raw) == SIGIL_OK) {
        sigil_card_listing *l = NULL;
        if (sigil_saturn_list(&small, &l) == SIGIL_OK) room = l->free_blocks;
        sigil_card_listing_free(l);
        sigil_saturn_volume_free(&small);
    }
    if (!unit || !need || !room || need <= room) {
        fail("too big for internal", "setup failed");
    } else {
        static const char *const LAYOUTS[] = { "yabause", "kronos", "mednafen_saturn", "mednafen_saturn shared" };
        for (size_t i = 0; i < 4; i++) {
            mem_root root = {0};
            game g;
            if (i == 3) {
                make_shared(&g, &root, "Touge King the Spirits (Japan).cue", NULL, 0, SIGIL_SYNC_MANAGED);
            } else {
                make_game(&g, &root, LAYOUTS[i], "saturn", "Touge King the Spirits (Japan).cue", SIGIL_SYNC_MANAGED);
            }
            sigil_sync_result *r = NULL;
            if (sigil_restore(&g.req, unit, unit_len, &r) != SIGIL_ERR_NO_SPACE || root.writes != 0 || !r ||
                strcmp(r->problem, "TGKRPLY_RP1") != 0 || r->blocks_short != need - room) {
                fail(LAYOUTS[i], "a save too big for 32 KiB of internal RAM wasn't refused with its shortfall");
            }
            sigil_sync_result_free(r);
            root_free(&root);
        }
    }
    free(unit);
    free(touge);
}

static void check_big_units_and_forms(void) {
    size_t yaba_len = 0, touge_len = 0;
    uint8_t *yaba = sample(&g_saturn, "saturn", "yabasanshiro-backup", NULL, &yaba_len);
    uint8_t *touge = sample(&g_saturn, "saturn", "touge-king-bup", NULL, &touge_len);
    sigil_io *io = yaba ? mem_root_io(yaba, yaba_len) : NULL;
    sigil_saturn_volume vol;
    uint8_t *with = NULL;
    size_t with_len = 0;
    if (io && touge && sigil_saturn_volume_load_internal(io, &vol) == SIGIL_OK) {
        if (sigil_saturn_inject(&vol, touge, touge_len) != SIGIL_OK || sigil_saturn_volume_write(&vol, &with, &with_len) != SIGIL_OK) with = NULL;
        sigil_saturn_volume_free(&vol);
    }
    if (io) sigil_io_close(io);
    if (!with) {
        fail("big units", "setup failed");
    } else {
        mem_root root = {0};
        root_put(&root, "yabasanshiro/backup.bin", with, with_len);
        game g;
        make_game(&g, &root, "yabasanshiro", "saturn", "Touge King the Spirits (Japan).cue", SIGIL_SYNC_MANAGED);
        static const char *const TOUGE[] = { "TGKRPLY_RP1" };
        g.req.claimed = TOUGE;
        g.req.claimed_count = 1;
        sigil_sync_result *r = NULL;
        if (sigil_collect(&g.req, &r) != SIGIL_OK || !r->data || r->len <= SATURN_INTERNAL_SIZE) {
            fail("big units", "a game's saves too big for 32 KiB didn't travel in a volume of the source's size");
        } else if (!r->holding) {
            fail("big units", "the other games' saves weren't held");
        } else {
            sigil_zip_member *m = NULL;
            size_t n = 0;
            if (sigil_zip_read_mem(r->holding, r->holding_len, 1u << 24, &m, &n) != SIGIL_OK || n != 1 ||
                m[0].len != r->len) {
                fail("big units", "the holding unit isn't the source volume's size");
            }
            sigil_zip_members_free(m, n);
        }
        sigil_sync_result_free(r);
        root_free(&root);
    }
    free(with);
    free(yaba);

    size_t toki_len = 0;
    uint8_t *toki = sample(&g_saturn, "saturn", "tokimeki-bup", "TOKIMEKI_99.BUP", &toki_len);
    sigil_saturn_volume small;
    sigil_bram_storage expanded;
    memset(&expanded, 0, sizeof(expanded));
    expanded.expanded = true;
    expanded.filler = 0xFF;
    uint8_t *few = NULL;
    size_t few_len = 0;
    if (toki && touge && sigil_saturn_volume_format(&small, 4u * 1024u * 1024u, &expanded) == SIGIL_OK) {
        if (sigil_saturn_inject(&small, touge, touge_len) != SIGIL_OK || sigil_saturn_inject(&small, toki, toki_len) != SIGIL_OK ||
            sigil_saturn_volume_write(&small, &few, &few_len) != SIGIL_OK) {
            few = NULL;
        }
        sigil_saturn_volume_free(&small);
    }
    if (!few) {
        fail("big units", "setup failed");
    } else {
        mem_root root = {0};
        root_put(&root, "yabasanshiro/backup.bin", few, few_len);
        game g;
        make_game(&g, &root, "yabasanshiro", "saturn", "Touge King the Spirits (Japan).cue", SIGIL_SYNC_MANAGED);
        static const char *const TOUGE[] = { "TGKRPLY_RP1" };
        g.req.claimed = TOUGE;
        g.req.claimed_count = 1;
        sigil_sync_result *r = NULL;
        sigil_zip_member *m = NULL;
        size_t n = 0;
        if (sigil_collect(&g.req, &r) != SIGIL_OK || !r->holding ||
            sigil_zip_read_mem(r->holding, r->holding_len, 1u << 24, &m, &n) != SIGIL_OK || n != 1 ||
            m[0].len != 4u * 1024u * 1024u) {
            fail("big units", "a holding unit of a few small saves isn't the 4 MiB source volume's size");
        }
        sigil_zip_members_free(m, n);
        sigil_sync_result_free(r);
        root_free(&root);
    }
    free(few);
    free(toki);
    free(touge);

    size_t hd_len = 0;
    uint8_t *hd = sample(&g_saturn, "saturn", "hyper-duel-bkr", NULL, &hd_len);
    if (!hd) return;
    mem_root root = {0};
    root_put(&root, "Hyper Duel (Japan).srm", hd, hd_len);
    game g;
    make_game(&g, &root, "yabause", "saturn", "Hyper Duel (Japan).cue", SIGIL_SYNC_MANAGED);
    sigil_sync_result *r = NULL, *placed = NULL;
    mem_file *f = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK || !r->data) {
        fail("saturn forms", "collect failed");
    } else {
        use_state(&g, r);
        g.req.overwrite_local = 1;
        root.writes = 0;
        sigil_io *vio = mem_root_io(hd, hd_len);
        sigil_saturn_volume hv;
        uint8_t *bup = NULL, *newer = NULL, *other = NULL;
        size_t bup_len = 0, other_len = 0;
        if (sigil_saturn_volume_load(vio, &hv) == SIGIL_OK) {
            sigil_card_listing *l = NULL;
            if (sigil_saturn_list(&hv, &l) == SIGIL_OK && l->entry_count) {
                sigil_saturn_extract(&hv, l->entries[0].first_block, &bup, &bup_len);
            }
            sigil_card_listing_free(l);
            sigil_saturn_volume_free(&hv);
        }
        sigil_io_close(vio);
        if (bup) newer = bup_variant(bup, bup_len, 1);
        if (newer) other = unit_of(newer, bup_len, &other_len);
        if (!other || sigil_restore(&g.req, other, other_len, &placed) != SIGIL_OK || root.writes == 0 ||
            !(f = root_find(&root, "Hyper Duel (Japan).srm")) || f->len != hd_len) {
            fail("saturn forms", "a raw file the core already has was rewritten in the layout's expanded form");
        }
        free(other);
        free(newer);
        free(bup);
    }
    sigil_sync_result_free(placed);
    sigil_sync_result_free(r);
    root_free(&root);
    free(hd);
}

/* A new cart file is the size the core's option names, whatever size the
 * unit's cart was: a core reads a cart of another size under that name
 * wrongly. */
static void check_cart_follows_option(void) {
    static const struct { size_t from; const char *option; const char *path; size_t size; } KRONOS[] = {
        { 512u * 1024u, "1M_backup_ram", "kronos/saturn/" RAYMAN "-ext1M.ram", 1024u * 1024u },
        { 4096u * 1024u, "512K_backup_ram", "kronos/saturn/" RAYMAN "-ext512K.ram", 512u * 1024u },
    };
    for (size_t i = 0; i < sizeof(KRONOS) / sizeof(KRONOS[0]); i++) {
        size_t cart_len = 0;
        uint8_t *cart = rayman_cart_of(KRONOS[i].from, &cart_len);
        if (!cart) { fail("cart size", "setup failed"); continue; }
        mem_root root = {0};
        root_put(&root, KRONOS[i].from == 512u * 1024u ? "kronos/saturn/" RAYMAN "-ext512K.ram"
                                                         : "kronos/saturn/" RAYMAN "-ext4M.ram", cart, cart_len);
        game g;
        make_game(&g, &root, "kronos", "saturn", RAYMAN ".cue", SIGIL_SYNC_MANAGED);
        if (KRONOS[i].from != 512u * 1024u) add_option(&g, "kronos_addon_cartridge", "4M_backup_ram");
        sigil_sync_result *unit = NULL, *placed = NULL, *back = NULL;
        mem_root empty = {0};
        game fresh;
        make_game(&fresh, &empty, "kronos", "saturn", RAYMAN ".cue", SIGIL_SYNC_MANAGED);
        add_option(&fresh, "kronos_addon_cartridge", KRONOS[i].option);
        mem_file *made = NULL;
        if (sigil_collect(&g.req, &unit) != SIGIL_OK || !unit->data) {
            fail("cart size", "kronos collect failed");
        } else if (sigil_restore(&fresh.req, unit->data, unit->len, &placed) != SIGIL_OK ||
                   !(made = root_find(&empty, KRONOS[i].path)) || made->len != KRONOS[i].size) {
            fail("cart size", "the Kronos cart isn't the size its option names");
        } else {
            refresh(&fresh, &empty);
            use_state(&fresh, placed);
            if (sigil_collect(&fresh.req, &back) != SIGIL_OK || strcmp(back->identity_hash, unit->identity_hash) != 0) {
                fail("cart size", "the Kronos saves differ on the resized cart");
            }
        }
        sigil_sync_result_free(back);
        sigil_sync_result_free(placed);
        sigil_sync_result_free(unit);
        root_free(&empty);
        root_free(&root);
        free(cart);
    }

    size_t popful_len = 0;
    uint8_t *popful = segacd("popful-mail-cart-brm", &popful_len);
    if (!popful) return;
    const char *content = "Popful Mail (USA) (RE).cue";
    mem_root root = {0};
    root_put(&root, "Popful Mail (USA) (RE)_4Mbit_cart.brm", popful, popful_len);
    game g;
    make_game(&g, &root, "genesis_plus_gx", "segacd", content, SIGIL_SYNC_MANAGED);
    add_option(&g, "genesis_plus_gx_cart_bram", "per game");
    sigil_sync_result *unit = NULL, *placed = NULL, *back = NULL;
    mem_root empty = {0};
    game fresh;
    make_game(&fresh, &empty, "genesis_plus_gx", "segacd", content, SIGIL_SYNC_MANAGED);
    add_option(&fresh, "genesis_plus_gx_cart_bram", "per game");
    add_option(&fresh, "genesis_plus_gx_cart_size", "1meg");
    mem_file *made = NULL;
    if (sigil_collect(&g.req, &unit) != SIGIL_OK || !unit->data) {
        fail("cart size", "sega cd collect failed");
    } else if (sigil_restore(&fresh.req, unit->data, unit->len, &placed) != SIGIL_OK ||
               !(made = root_find(&empty, "Popful Mail (USA) (RE)_1Mbit_cart.brm")) || made->len != 128u * 1024u) {
        fail("cart size", "the Sega CD cart isn't the size its option names");
    } else {
        refresh(&fresh, &empty);
        use_state(&fresh, placed);
        if (sigil_collect(&fresh.req, &back) != SIGIL_OK || strcmp(back->identity_hash, unit->identity_hash) != 0) {
            fail("cart size", "the Sega CD saves differ on the resized cart");
        }
    }
    sigil_sync_result_free(back);
    sigil_sync_result_free(placed);
    sigil_sync_result_free(unit);
    root_free(&empty);
    root_free(&root);
    free(popful);
}

/* A game with saves on its cart alone travels as a zip, so the unit names
 * the device whatever the cart's size. */
static void check_cart_only_unit_is_zip(void) {
    size_t cart_len = 0;
    uint8_t *cart = rayman_cart_of(4096u * 1024u, &cart_len);
    if (!cart) { fail("cart only", "setup failed"); return; }
    mem_root root = {0};
    root_put(&root, "kronos/saturn/" RAYMAN "-ext4M.ram", cart, cart_len);
    game g;
    make_game(&g, &root, "kronos", "saturn", RAYMAN ".cue", SIGIL_SYNC_MANAGED);
    add_option(&g, "kronos_addon_cartridge", "4M_backup_ram");
    sigil_sync_result *r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK || r->shape != SIGIL_SAVE_SHAPE_MULTI || strcmp(r->artifact, RAYMAN ".zip") != 0) {
        fail("cart only", "a cart-only unit isn't a zip");
    }
    sigil_sync_result_free(r);
    root_free(&root);
    free(cart);
}

/* Yaba Sanshiro keeps every game in one expanded backup.bin. A swap keeps
 * the file's size and form. */
static void check_yabasanshiro(void) {
    size_t len = 0;
    uint8_t *shared = sample(&g_saturn, "saturn", "yabasanshiro-backup", NULL, &len);
    if (!shared) return;
    mem_root root = {0};
    root_put(&root, "yabasanshiro/backup.bin", shared, len);
    game g;
    make_game(&g, &root, "yabasanshiro", "saturn", "Some Game (USA).cue", SIGIL_SYNC_MANAGED);
    sigil_sync_result *held = NULL, *claimed = NULL, *r = NULL;
    if (sigil_collect(&g.req, &held) != SIGIL_OK || !held->holding || held->unowned_count == 0) {
        fail("yabasanshiro", "the shared backup.bin wasn't held back");
    } else {
        const char *claim[] = { held->unowned[0] };
        g.req.claimed = claim;
        g.req.claimed_count = 1;
        if (sigil_collect(&g.req, &claimed) != SIGIL_OK || !claimed->data || claimed->shape != SIGIL_SAVE_SHAPE_SINGLE) {
            fail("yabasanshiro", "a claimed save didn't make a unit");
        } else {
            g.req.claimed = NULL;
            g.req.claimed_count = 0;
            use_state(&g, NULL);
            if (sigil_restore(&g.req, claimed->data, claimed->len, &r) != SIGIL_ERR_UNCOLLECTED || root.writes != 0) {
                fail("yabasanshiro", "a swap ran over saves no holding unit passed on");
            }
            sigil_sync_result_free(r);
            r = NULL;
            use_state(&g, claimed);
            if (sigil_restore(&g.req, claimed->data, claimed->len, &r) != SIGIL_OK || root.writes != 1) {
                fail("yabasanshiro", "restore after the holding unit went up failed");
            } else {
                mem_file *f = root_find(&root, "yabasanshiro/backup.bin");
                sigil_card_listing *l = listing_of(f->data, f->len);
                if (f->len != len || !l || l->entry_count != 1 || !has_name(l, claim[0])) {
                    fail("yabasanshiro", "the swapped backup.bin lost its form or holds other saves");
                }
                sigil_card_listing_free(l);
            }
            mem_root empty = {0};
            game fresh;
            make_game(&fresh, &empty, "yabasanshiro", "saturn", "Some Game (USA).cue", SIGIL_SYNC_MANAGED);
            sigil_sync_result *first = NULL;
            if (sigil_restore(&fresh.req, claimed->data, claimed->len, &first) != SIGIL_OK) {
                fail("yabasanshiro", "restore into an empty root failed");
            } else {
                mem_file *f = root_find(&empty, "yabasanshiro/backup.bin");
                if (!f || f->len != 8388608 || f->data[0] != 0xFF || f->data[1] != 'B') {
                    fail("yabasanshiro", "the new backup.bin isn't the core's 8 MiB expanded form");
                }
            }
            sigil_sync_result_free(first);
            root_free(&empty);
        }
    }
    sigil_sync_result_free(r);
    sigil_sync_result_free(claimed);
    sigil_sync_result_free(held);
    root_free(&root);
    free(shared);
}

/* The yabause core keeps a 64 KiB byte-expanded .srm. A restore into an empty
 * root writes that form, not the raw 32 KiB volume the unit carries. */
static void check_yabause(void) {
    size_t len = 0;
    const char *stem = "Akumajou Dracula X - Gekka no Yasoukyoku (Japan) (2M)";
    char srm[256], cue[256];
    snprintf(srm, sizeof(srm), "%s.srm", stem);
    snprintf(cue, sizeof(cue), "%s.cue", stem);
    uint8_t *vol = sample(&g_saturn, "saturn", "draculax-yabause-srm", NULL, &len);
    if (!vol) return;
    mem_root root = {0};
    root_put(&root, srm, vol, len);
    game g;
    make_game(&g, &root, "yabause", "saturn", cue, SIGIL_SYNC_MANAGED);
    sigil_sync_result *r = NULL, *restored = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK || !r->data || r->len != 32768) {
        fail("yabause", "collect didn't make a raw internal unit");
    } else {
        mem_root empty = {0};
        game fresh;
        make_game(&fresh, &empty, "yabause", "saturn", cue, SIGIL_SYNC_MANAGED);
        if (sigil_restore(&fresh.req, r->data, r->len, &restored) != SIGIL_OK) {
            fail("yabause", "restore into an empty root failed");
        } else {
            mem_file *f = root_find(&empty, srm);
            if (!f || f->len != 65536 || f->data[0] != 0xFF || f->data[1] != 'B' || f->data[0xF000] != 0xFF) {
                fail("yabause", "the new .srm isn't the core's 64 KiB expanded form");
            }
        }
        root_free(&empty);
    }
    sigil_sync_result_free(restored);
    sigil_sync_result_free(r);
    root_free(&root);
    free(vol);
}

/* A save the name table gives the game is the game's on a shared volume
 * nobody claimed; other games' saves stay held, and a game without a row
 * claims nothing. */
static void check_name_table(void) {
    size_t len = 0;
    uint8_t *shared = sample(&g_saturn, "saturn", "yabasanshiro-backup", NULL, &len);
    if (!shared) return;
    mem_root root = {0};
    root_put(&root, "yabasanshiro/backup.bin", shared, len);
    game g;
    make_game(&g, &root, "yabasanshiro", "saturn", "Panzer Dragoon II Zwei (USA).cue", SIGIL_SYNC_MANAGED);
    const char *const zwei[] = { "MK-81022" };
    g.req.game_ids = zwei;
    g.req.game_id_count = 1;
    sigil_sync_result *r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK || !r->data) {
        fail("name table", "the table didn't give Zwei its save");
    } else {
        sigil_card_listing *l = listing_of(r->data, r->len);
        if (!l || l->entry_count != 1 || !has_name(l, "PANDRA_ZWEI")) fail("name table", "the unit isn't Zwei's save alone");
        for (size_t i = 0; i < r->unowned_count; i++) {
            if (strcmp(r->unowned[i], "PANDRA_ZWEI") == 0) fail("name table", "Zwei's save is also held");
        }
        sigil_card_listing_free(l);
    }
    sigil_sync_result_free(r);
    r = NULL;

    const char *const unknown[] = { "T-99999" };
    g.req.game_ids = unknown;
    if (sigil_collect(&g.req, &r) != SIGIL_OK || r->data) fail("name table", "a game without a row claimed a save");
    sigil_sync_result_free(r);
    root_free(&root);
    free(shared);

    size_t dw_len = 0;
    uint8_t *dw = segacd("dark-wizard-brm", &dw_len);
    if (dw) {
        mem_root scd = {0};
        root_put(&scd, "scd_J.brm", dw, dw_len);
        game d;
        make_game(&d, &scd, "genesis_plus_gx", "segacd", "Dark Wizard (Japan).cue", SIGIL_SYNC_MANAGED);
        const char *const header[] = { "GM G-6005  -00" };
        d.req.game_ids = header;
        d.req.game_id_count = 1;
        r = NULL;
        if (sigil_collect(&d.req, &r) != SIGIL_OK || !r->data || r->holding) {
            fail("name table", "Dark Wizard's saves weren't both claimed by the Sega CD row");
        }
        sigil_sync_result_free(r);
        root_free(&scd);
        free(dw);
    }
}

/* ---- Sega CD shared volumes ------------------------------------------------------ */

/* A shared volume nobody claimed yet holds every save back for the user, and
 * the game's unit is empty. A claim moves a save into the unit. */
static void check_segacd_holding_and_claims(sigil_sync_result **held_state) {
    size_t len = 0;
    uint8_t *multi = segacd("multi-titles-brm", &len);
    if (!multi) return;
    mem_root root = {0};
    root_put(&root, "scd_U.brm", multi, len);
    game g;
    make_game(&g, &root, "genesis_plus_gx", "segacd", LUNAR, SIGIL_SYNC_MANAGED);
    sigil_sync_result *r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK) {
        fail("segacd holding", "collect failed");
    } else {
        if (r->data) fail("segacd holding", "saves with no owner went into the game's unit");
        if (!r->holding || r->unowned_count != entry_count(multi, len)) fail("segacd holding", "not every save was held back");
        sigil_zip_member *m = NULL;
        size_t n = 0;
        if (!r->holding || sigil_zip_read_mem(r->holding, r->holding_len, 1u << 24, &m, &n) != SIGIL_OK || n != 1 ||
            strcmp(m[0].name, "backup.ram") != 0 || entry_count(m[0].data, m[0].len) != r->unowned_count) {
            fail("segacd holding", "the holding unit isn't a zip of the held saves");
        }
        sigil_zip_members_free(m, n);
        *held_state = r;
        r = NULL;
    }

    const char *const claim[] = { "SFCD_DAT_09" };
    g.req.claimed = claim;
    g.req.claimed_count = 1;
    if (sigil_collect(&g.req, &r) != SIGIL_OK || !r->data) {
        fail("segacd claims", "a claimed save didn't make a unit");
    } else {
        sigil_card_listing *l = listing_of(r->data, r->len);
        if (!l || l->entry_count != 1 || !has_name(l, "SFCD_DAT_09")) fail("segacd claims", "the unit isn't the claimed save");
        if (r->unowned_count != entry_count(multi, len) - 1) fail("segacd claims", "the claimed save is still held back");
        sigil_zip_member *m = NULL;
        size_t n = 0;
        sigil_card_listing *held = NULL;
        if (!r->holding || sigil_zip_read_mem(r->holding, r->holding_len, 1u << 24, &m, &n) != SIGIL_OK || n != 1 ||
            !(held = listing_of(m[0].data, m[0].len)) || has_name(held, "SFCD_DAT_09")) {
            fail("segacd claims", "the holding unit still carries the claimed save");
        }
        sigil_card_listing_free(held);
        sigil_zip_members_free(m, n);
        sigil_card_listing_free(l);
    }
    sigil_sync_result_free(r);
    root_free(&root);
    free(multi);
}

/* Managed restore swaps the shared volume only once its saves were passed on;
 * after the swap the volume holds the game's saves alone, and a save the game
 * writes during the session is the game's. */
static void check_segacd_swap(const sigil_sync_result *held_state, const sigil_sync_result *lunar) {
    size_t len = 0;
    uint8_t *multi = segacd("multi-titles-brm", &len);
    if (!multi || !held_state || !lunar) { free(multi); fail("segacd swap", "setup failed"); return; }
    mem_root root = {0};
    root_put(&root, "scd_U.brm", multi, len);
    game g;
    make_game(&g, &root, "genesis_plus_gx", "segacd", LUNAR, SIGIL_SYNC_MANAGED);

    sigil_sync_result *r = NULL;
    if (sigil_restore(&g.req, lunar->data, lunar->len, &r) != SIGIL_ERR_UNCOLLECTED || root.writes != 0) {
        fail("segacd swap", "a volume with saves nobody passed on was swapped");
    }
    sigil_sync_result_free(r);
    r = NULL;

    use_state(&g, held_state);
    if (sigil_restore(&g.req, lunar->data, lunar->len, &r) != SIGIL_OK || root.writes != 1) {
        fail("segacd swap", "restore after the holding unit was passed on failed");
        sigil_sync_result_free(r);
        root_free(&root);
        free(multi);
        return;
    }
    mem_file *f = root_find(&root, "scd_U.brm");
    sigil_card_listing *l = listing_of(f->data, f->len);
    if (!l || l->entry_count != 1 || !has_name(l, "GA_LUNAR_01")) fail("segacd swap", "the swapped volume doesn't hold Lunar's save alone");
    sigil_card_listing_free(l);

    size_t save_len = 0, grown_len = 0;
    uint8_t *save = segacd_save(multi, len, "SFCD_DAT_09", &save_len);
    uint8_t *grown = save ? segacd_with(f->data, f->len, save, save_len, &grown_len) : NULL;
    if (!grown) {
        fail("segacd swap", "setup of a session write failed");
    } else {
        root_put(&root, "scd_U.brm", grown, grown_len);
        use_state(&g, r);
        sigil_sync_result *after = NULL;
        if (sigil_collect(&g.req, &after) != SIGIL_OK || !after->data || !after->changed || after->holding) {
            fail("segacd swap", "a save written during the session isn't the game's");
        } else {
            sigil_card_listing *ul = listing_of(after->data, after->len);
            if (!ul || ul->entry_count != 2) fail("segacd swap", "the unit misses the new save");
            sigil_card_listing_free(ul);
        }
        sigil_sync_result_free(after);
    }
    free(save);
    free(grown);
    sigil_sync_result_free(r);
    root_free(&root);
    free(multi);
}

/* Another game's saves on a shared volume block a swap until that game's
 * collect matches them. */
static void check_segacd_other_game(const sigil_sync_result *lunar) {
    size_t len = 0;
    uint8_t *multi = segacd("multi-titles-brm", &len);
    if (!multi || !lunar) { free(multi); return; }
    mem_root root = {0};
    root_put(&root, "scd_U.brm", multi, len);
    game sonic;
    make_game(&sonic, &root, "genesis_plus_gx", "segacd", "Sonic CD (USA).cue", SIGIL_SYNC_MANAGED);
    const char *const everything[] = { "POPFUL_MAIL", "DW__DATA_00", "SFCD_DAT_09", "SONICCD__01", "SONICCD__02",
                                       "SONICCD____", "SONICCD__03" };
    sonic.req.claimed = everything;
    sonic.req.claimed_count = 7;
    sigil_sync_result *collected = NULL, *r = NULL;
    if (sigil_collect(&sonic.req, &collected) != SIGIL_OK || !collected->data) {
        fail("segacd other game", "setup collect failed");
    } else {
        game lunar_game;
        make_game(&lunar_game, &root, "genesis_plus_gx", "segacd", LUNAR, SIGIL_SYNC_MANAGED);
        use_state(&lunar_game, collected);
        if (sigil_restore(&lunar_game.req, lunar->data, lunar->len, &r) != SIGIL_OK) {
            fail("segacd other game", "another game's collected saves blocked the swap");
        }
        sigil_sync_result_free(r);
        r = NULL;

        size_t dw_len = 0, newer_len = 0, changed_len = 0;
        uint8_t *dw = segacd("dark-wizard-brm", &dw_len);
        uint8_t *newer = dw ? segacd_save(dw, dw_len, "DW__DATA_00", &newer_len) : NULL;
        uint8_t *changed = newer ? segacd_replaced(multi, len, "DW__DATA_00", newer, newer_len, &changed_len) : NULL;
        if (!changed) {
            fail("segacd other game", "setup of an uncollected save failed");
        } else {
            root_put(&root, "scd_U.brm", changed, changed_len);
            int writes = root.writes;
            if (sigil_restore(&lunar_game.req, lunar->data, lunar->len, &r) != SIGIL_ERR_UNCOLLECTED || root.writes != writes) {
                fail("segacd other game", "another game's save that changed since its collect was swapped away");
            }
            sigil_sync_result_free(r);
            r = NULL;
        }
        free(changed);
        free(newer);
        free(dw);
    }
    sigil_sync_result_free(collected);
    root_free(&root);
    free(multi);
}

/* The core picks scd_E, scd_U or scd_J by the disc's region: sigil reads a
 * forced region option first, then the content name's region, then the only
 * file there. */
static void check_segacd_region(void) {
    size_t len = 0;
    uint8_t *multi = segacd("multi-titles-brm", &len);
    if (!multi) return;
    mem_root root = {0};
    root_put(&root, "scd_J.brm", multi, len);
    struct { const char *content; const char *region; bool holds; int rc; } CASES[] = {
        { "Lunar (Japan).cue", NULL, true, SIGIL_OK },
        { "Lunar (USA).cue", NULL, false, SIGIL_OK },
        { "Lunar (USA).cue", "ntsc-j", true, SIGIL_OK },
        { "Lunar.cue", NULL, true, SIGIL_OK },
    };
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        game g;
        make_game(&g, &root, "genesis_plus_gx", "segacd", CASES[i].content, SIGIL_SYNC_MANAGED);
        if (CASES[i].region) add_option(&g, "genesis_plus_gx_region_detect", CASES[i].region);
        sigil_sync_result *r = NULL;
        int rc = sigil_collect(&g.req, &r);
        if (rc != CASES[i].rc || (rc == SIGIL_OK && (r->holding != NULL) != CASES[i].holds)) {
            fail("segacd region", CASES[i].content);
        }
        sigil_sync_result_free(r);
    }
    root_put(&root, "scd_E.brm", multi, len);
    game g;
    make_game(&g, &root, "genesis_plus_gx", "segacd", "Lunar.cue", SIGIL_SYNC_MANAGED);
    sigil_sync_result *r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_ERR_NOT_FOUND) fail("segacd region", "two region files and no region didn't refuse");
    sigil_sync_result_free(r);
    root_free(&root);
    free(multi);
}

/* A file at a volume's path that isn't that volume (an 8 KiB internal
 * volume where the 4 Mbit cart goes) is damaged, never formatted over:
 * collect and restore refuse naming it, with or without repair. */
static void check_unreadable_volume(const sigil_sync_result *lunar) {
    size_t blank_len = 0;
    uint8_t *blank = segacd("blank-internal", &blank_len);
    if (!blank || !lunar) { fail("unreadable volume", "setup failed"); free(blank); return; }
    for (int repair = 0; repair < 2; repair++) {
        mem_root root = {0};
        root_put(&root, "4Mbit_cart.brm", blank, blank_len);
        game g;
        make_game(&g, &root, "genesis_plus_gx", "segacd", LUNAR, SIGIL_SYNC_MANAGED);
        add_option(&g, "genesis_plus_gx_system_bram", "per game");
        g.req.repair = repair;
        g.req.overwrite_local = 1;
        sigil_sync_result *seen = NULL, *r = NULL;
        if (sigil_collect(&g.req, &seen) != SIGIL_ERR_DAMAGED || !seen || strcmp(seen->problem, "4Mbit_cart.brm") != 0) {
            fail("unreadable volume", "collect didn't refuse naming the file that isn't the cart");
        }
        if (sigil_restore(&g.req, lunar->data, lunar->len, &r) != SIGIL_ERR_DAMAGED || root.writes != 0) {
            fail("unreadable volume", "restore wrote over a file that isn't the cart");
        }
        sigil_sync_result_free(r);
        sigil_sync_result_free(seen);
        root_free(&root);
    }
    free(blank);
}

/* Unmanaged into a shared volume that already holds the game's saves: the
 * game's saves the unit lacks go, the others are replaced, and another
 * game's save stays, though each Sega CD delete moves the saves after it. */
static void check_segacd_unmanaged_replace(void) {
    size_t dw_len = 0, lunar_len = 0, blank_len = 0;
    uint8_t *dw = segacd("dark-wizard-brm", &dw_len), *lunar = segacd("lunar-ecc-brm", &lunar_len);
    uint8_t *blank = segacd("blank-internal", &blank_len);
    size_t ga_len = 0, vol_len = 0, dw0_len = 0, unit_len = 0;
    uint8_t *ga = lunar ? segacd_save(lunar, lunar_len, "GA_LUNAR_01", &ga_len) : NULL;
    uint8_t *vol = dw && ga ? segacd_with(dw, dw_len, ga, ga_len, &vol_len) : NULL;
    uint8_t *dw0 = dw ? segacd_save(dw, dw_len, "DW__DATA_00", &dw0_len) : NULL;
    uint8_t *unit = blank && dw0 ? segacd_with(blank, blank_len, dw0, dw0_len, &unit_len) : NULL;
    mem_root root = {0};
    sigil_sync_result *seen = NULL, *r = NULL;
    if (!vol || !unit) { fail("segacd replace", "setup failed"); goto done; }
    root_put(&root, "scd_U.brm", vol, vol_len);
    static const char *const DW_IDS[] = { "G-6005" };
    game g;
    make_game(&g, &root, "genesis_plus_gx", "segacd", "Dark Wizard (USA).cue", SIGIL_SYNC_UNMANAGED);
    g.req.game_ids = DW_IDS;
    g.req.game_id_count = 1;
    if (sigil_collect(&g.req, &seen) != SIGIL_OK || !seen->data) { fail("segacd replace", "collect failed"); goto done; }
    use_state(&g, seen);
    mem_file *f = NULL;
    sigil_card_listing *l = NULL;
    if (sigil_restore(&g.req, unit, unit_len, &r) != SIGIL_OK || !(f = root_find(&root, "scd_U.brm"))) {
        fail("segacd replace", "restore failed");
    } else if (!(l = listing_of(f->data, f->len)) || l->entry_count != 2 || !has_name(l, "DW__DATA_00") ||
               has_name(l, "DW__DATA_01") || !has_name(l, "GA_LUNAR_01")) {
        fail("segacd replace", "the game's saves weren't replaced exactly, or another game's save went");
    }
    sigil_card_listing_free(l);
done:
    sigil_sync_result_free(r);
    sigil_sync_result_free(seen);
    root_free(&root);
    free(unit); free(dw0); free(vol); free(ga); free(blank); free(lunar); free(dw);
}

/* Unmanaged: restore injects beside other saves only when the volume is as
 * the last collect saw it, and a core that overwrites the injected save on
 * unload makes the next collect ask for the restore again. */
static void check_segacd_unmanaged(const sigil_sync_result *lunar) {
    size_t len = 0, dw_len = 0, blank_len = 0, save_len = 0, small_len = 0;
    uint8_t *multi = segacd("multi-titles-brm", &len);
    uint8_t *dw = segacd("dark-wizard-brm", &dw_len);
    uint8_t *blank = segacd("blank-internal", &blank_len);
    uint8_t *save = multi ? segacd_save(multi, len, "SFCD_DAT_09", &save_len) : NULL;
    uint8_t *small = blank && save ? segacd_with(blank, blank_len, save, save_len, &small_len) : NULL;
    if (!lunar || !dw || !small) {
        fail("segacd unmanaged", "setup failed");
        free(multi); free(dw); free(blank); free(save); free(small);
        return;
    }
    const char *sfcd = "Shining Force CD (USA).cue";
    mem_root own = {0};
    root_put(&own, "Shining Force CD (USA).brm", small, small_len);
    game from;
    make_game(&from, &own, "genesis_plus_gx", "segacd", sfcd, SIGIL_SYNC_UNMANAGED);
    add_option(&from, "genesis_plus_gx_system_bram", "per game");
    sigil_sync_result *unit = NULL;
    if (sigil_collect(&from.req, &unit) != SIGIL_OK || !unit->data) {
        fail("segacd unmanaged", "setup collect failed");
        sigil_sync_result_free(unit);
        root_free(&own);
        free(multi); free(dw); free(blank); free(save); free(small);
        return;
    }

    mem_root root = {0};
    root_put(&root, "scd_U.brm", dw, dw_len);
    game g;
    make_game(&g, &root, "genesis_plus_gx", "segacd", sfcd, SIGIL_SYNC_UNMANAGED);
    sigil_sync_result *r = NULL;
    if (sigil_restore(&g.req, unit->data, unit->len, &r) != SIGIL_ERR_UNCOLLECTED || root.writes != 0) {
        fail("segacd unmanaged", "a volume no collect saw took an inject");
    }
    sigil_sync_result_free(r);
    r = NULL;

    sigil_sync_result *seen = NULL;
    if (sigil_collect(&g.req, &seen) != SIGIL_OK) {
        fail("segacd unmanaged", "collect failed");
    } else {
        use_state(&g, seen);
        if (sigil_restore(&g.req, unit->data, unit->len, &r) != SIGIL_OK || root.writes != 1) {
            fail("segacd unmanaged", "restore beside other saves failed");
        } else {
            mem_file *f = root_find(&root, "scd_U.brm");
            sigil_card_listing *l = listing_of(f->data, f->len);
            if (!l || l->entry_count != entry_count(dw, dw_len) + 1 || !has_name(l, "SFCD_DAT_09") ||
                !has_name(l, "DW__DATA_00") || !has_name(l, "DW__DATA_01")) {
                fail("segacd unmanaged", "the inject didn't keep every other save");
            }
            sigil_card_listing_free(l);

            root_put(&root, "scd_U.brm", dw, dw_len);
            use_state(&g, r);
            sigil_sync_result *after = NULL;
            if (sigil_collect(&g.req, &after) != SIGIL_OK || !after->restore_again || after->changed) {
                fail("segacd unmanaged", "an overwritten inject didn't ask for the restore again");
            }
            sigil_sync_result_free(after);
        }
        sigil_sync_result_free(r);
        r = NULL;

        mem_root full = {0};
        root_put(&full, "scd_U.brm", multi, len);
        game crowded;
        make_game(&crowded, &full, "genesis_plus_gx", "segacd", LUNAR, SIGIL_SYNC_UNMANAGED);
        sigil_sync_result *looked = NULL;
        if (sigil_collect(&crowded.req, &looked) == SIGIL_OK) {
            use_state(&crowded, looked);
            if (sigil_restore(&crowded.req, lunar->data, lunar->len, &r) != SIGIL_ERR_NO_SPACE || full.writes != 0) {
                fail("segacd unmanaged", "a save too big for the volume was written");
            }
            sigil_sync_result_free(r);
        }
        sigil_sync_result_free(looked);
        root_free(&full);
    }
    sigil_sync_result_free(seen);
    sigil_sync_result_free(unit);
    root_free(&root);
    root_free(&own);
    free(multi); free(dw); free(blank); free(save); free(small);
}

int main(void) {
    char path[1024];
    if (corpus_platform_path("saturn", "manifest.tsv", path, sizeof(path)) != 0 || corpus_load(path, &g_saturn) != 0 ||
        corpus_platform_path("segacd", "manifest.tsv", path, sizeof(path)) != 0 || corpus_load(path, &g_segacd) != 0) {
        fprintf(stderr, "SKIP: no saturn or segacd manifest\n");
        return TEST_SKIP;
    }
    if (!corpus_present(&g_segacd, "segacd", "multi-titles-brm") || !corpus_present(&g_saturn, "saturn", "rayman-bkr-bcr")) {
        fprintf(stderr, "SKIP: volume samples missing\n");
        return TEST_SKIP;
    }

    check_saturn_internal_and_cart();
    check_saturn_keeps_gzip();
    check_saturn_single();
    check_saturn_legacy_bkr();
    check_kronos();
    check_cart_only_unit_is_zip();
    check_cart_follows_option();
    check_saturn_unmanaged();
    check_saturn_swap_guard();
    check_unopenable_volume();
    check_owner_precedence();
    check_saturn_companion_shared();
    check_big_units_and_forms();
    check_too_big_for_internal();
    check_cart_too_small();
    check_kronos_beetle_cross();
    check_volume_files();
    check_kept_companion_overflow();
    check_yabasanshiro();
    check_yabause();
    check_name_table();

    sigil_sync_result *lunar = lunar_unit();
    sigil_sync_result *held = NULL;
    check_segacd_holding_and_claims(&held);
    check_segacd_swap(held, lunar);
    check_segacd_other_game(lunar);
    check_segacd_region();
    check_segacd_unmanaged(lunar);
    check_segacd_unmanaged_replace();
    check_unreadable_volume(lunar);
    check_corrupt_saves(lunar);
    if (lunar) {
        check_faulty_write("segacd faulty write", "genesis_plus_gx", "segacd", LUNAR, "genesis_plus_gx_system_bram",
                           "per game", lunar->data, lunar->len, first_set_byte);
    }
    sigil_sync_result_free(held);
    sigil_sync_result_free(lunar);

    corpus_free(&g_saturn);
    corpus_free(&g_segacd);
    printf("volume sync: %d failures\n", g_fails);
    return corpus_exit(g_fails);
}
