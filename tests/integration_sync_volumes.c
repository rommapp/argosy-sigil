// SPDX-License-Identifier: MPL-2.0
#include "save_corpus.h"
#include "mem_root.h"
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
    for (size_t r = 0; r < manifest->nrows; r++) {
        if (strcmp(corpus_get(manifest, r, "id"), id) != 0) continue;
        if (path && strcmp(corpus_get(manifest, r, "path"), path) != 0) continue;
        char full[1024];
        if (corpus_sample_path(platform, id, corpus_get(manifest, r, "path"), full, sizeof(full)) != 0) return NULL;
        return corpus_read_file(full, len);
    }
    return NULL;
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
    size_t probe_len = 0;
    uint8_t *probe = segacd("multi-titles-brm", &probe_len);
    if (!probe) {
        fprintf(stderr, "SKIP: volume samples missing\n");
        return TEST_SKIP;
    }
    free(probe);

    check_saturn_internal_and_cart();
    check_saturn_keeps_gzip();
    check_saturn_single();
    check_kronos();
    check_cart_only_unit_is_zip();
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
    sigil_sync_result_free(held);
    sigil_sync_result_free(lunar);

    corpus_free(&g_saturn);
    corpus_free(&g_segacd);
    printf("volume sync: %d failures\n", g_fails);
    return g_fails ? 1 : 0;
}
