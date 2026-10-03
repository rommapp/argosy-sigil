// SPDX-License-Identifier: MPL-2.0
#include "save_corpus.h"
#include "mem_root.h"
#include "card_ps2.h"
#include "sync_internal.h"
#include <stdbool.h>

#define TEST_SKIP 77
#define FOLDER_FILES 16

static int g_fails = 0;

static void fail(const char *where, const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", where, what);
    g_fails++;
}

static corpus_table g_manifest;

/* ---- samples ------------------------------------------------------------------ */

static uint8_t *sample(const char *id, size_t *len) {
    return corpus_sample(&g_manifest, "ps2", id, NULL, len);
}

static bool load_card(const uint8_t *data, size_t len, sigil_ps2_card *card) {
    sigil_io *io = mem_root_io(data, len);
    int rc = sigil_ps2_card_load(io, card);
    sigil_io_close(io);
    return rc == SIGIL_OK;
}

/* One folder-card sample packed into a save, as PCSX2 would put it on a card. */
static bool folder_save(const char *id, sigil_ps2_save *out) {
    sigil_ps2_folder_file files[FOLDER_FILES];
    size_t n = 0;
    char folder[64] = "";
    for (size_t r = 0; r < g_manifest.nrows && n < FOLDER_FILES; r++) {
        if (strcmp(corpus_get(&g_manifest, r, "id"), id) != 0) continue;
        const char *path = corpus_get(&g_manifest, r, "path");
        const char *slash = strchr(path, '/');
        if (!slash) continue;
        snprintf(folder, sizeof(folder), "%.*s", (int)(slash - path), path);
        files[n].data = corpus_sample(&g_manifest, "ps2", id, path, &files[n].len);
        if (!files[n].data) continue;
        snprintf(files[n].path, sizeof(files[n].path), "%s", slash + 1);
        n++;
    }
    bool ok = n > 0 && sigil_ps2_pack(folder, files, n, out) == SIGIL_OK;
    for (size_t i = 0; i < n; i++) free(files[i].data);
    return ok;
}

static bool extract_named(const sigil_ps2_card *card, const char *name, sigil_ps2_save *out) {
    sigil_card_listing *l = NULL;
    bool ok = false;
    if (sigil_ps2_card_list(card, &l) != SIGIL_OK) return false;
    for (size_t i = 0; i < l->entry_count && !ok; i++) {
        if (strcmp(l->entries[i].name, name) == 0) ok = sigil_ps2_extract(card, l->entries[i].first_block, out) == SIGIL_OK;
    }
    sigil_card_listing_free(l);
    return ok;
}

static uint8_t *card_bytes(const sigil_ps2_card *card, size_t *len) {
    *len = sigil_ps2_card_file_size(card);
    uint8_t *out = (uint8_t *)malloc(*len);
    if (sigil_ps2_card_write(card, out) != SIGIL_OK) { free(out); return NULL; }
    return out;
}

#define ACE_COMBAT   "BASLUS-20152AC04"
#define ATHF         "BASLUS-21633-ATHF-1"
#define NORRATH      "BASLUS-20565"
#define REZ          "BESCES-50501REZ"
#define DATA_SYSTEM  "BEDATA-SYSTEM"

static const char *const ACE_IDS[] = { "SLUS-20152" };

/* A card shared by several games: Rez and the system folder from the mymc
 * sample, then three folder-card saves in the order given. */
static uint8_t *shared_card(const char *const *order, size_t count, size_t *len) {
    size_t mc01_len = 0;
    uint8_t *mc01 = sample("mymc-mc01", &mc01_len);
    sigil_ps2_card card;
    uint8_t *out = NULL;
    if (mc01 && load_card(mc01, mc01_len, &card)) {
        bool ok = true;
        for (size_t i = 0; i < count && ok; i++) {
            sigil_ps2_save save;
            ok = folder_save(order[i], &save) && sigil_ps2_inject(&card, &save) == SIGIL_OK;
            if (ok) sigil_ps2_save_free(&save);
        }
        if (ok) out = card_bytes(&card, len);
        sigil_ps2_card_free(&card);
    }
    free(mc01);
    return out;
}

static const char *const ORDER_A[] = { "ace-combat-04-aethersx2", "athf-aethersx2", "champions-of-norrath-aethersx2" };
static const char *const ORDER_B[] = { "champions-of-norrath-aethersx2", "athf-aethersx2", "ace-combat-04-aethersx2" };

/* ---- requests ----------------------------------------------------------------- */

typedef struct {
    sigil_sync_request req;
    sigil_result       result;
    sigil_save_option  option;
} game;

static void make_game(game *g, mem_root *root, const char *content, const char *title_id, bool per_content) {
    memset(g, 0, sizeof(*g));
    g->result.struct_version = SIGIL_RESULT_V3;
    g->result.platform = SIGIL_PLATFORM_PS2;
    snprintf(g->result.title_id, sizeof(g->result.title_id), "%s", title_id);
    g->req.struct_version = SIGIL_SYNC_REQUEST_V1;
    g->req.save.struct_version = SIGIL_SAVE_REQUEST_V1;
    g->req.save.layout = "pcsx2";
    g->req.save.platform = "ps2";
    g->req.save.content_path = content;
    g->req.save.result = &g->result;
    g->req.save.listing = root->listing;
    g->req.save.listing_count = root->count;
    g->req.save.open = root_open;
    g->req.save.open_ctx = root;
    g->req.write = root_write;
    g->req.write_ctx = root;
    if (per_content) {
        g->option.key = "pcsx2_shared_memory_cards";
        g->option.value = "disabled";
        g->req.save.options = &g->option;
        g->req.save.option_count = 1;
    }
}

static void refresh_listing(game *g, mem_root *root) {
    g->req.save.listing = root->listing;
    g->req.save.listing_count = root->count;
}

/* The unit is a PS2 card holding exactly the Ace Combat save, byte for byte
 * as PCSX2 packs the folder sample. */
static void check_unit_is_ace_combat(const char *where, const sigil_sync_result *r) {
    sigil_ps2_card unit;
    if (!r->data || !load_card(r->data, r->len, &unit)) { fail(where, "the unit is not a PS2 card"); return; }
    sigil_card_listing *l = NULL;
    sigil_ps2_save expected;
    if (sigil_ps2_card_list(&unit, &l) != SIGIL_OK || l->entry_count != 1 || strcmp(l->entries[0].name, ACE_COMBAT) != 0) {
        fail(where, "the unit holds other saves, or not the game's");
    }
    if (!folder_save("ace-combat-04-aethersx2", &expected) || sigil_ps2_verify(&unit, &expected) != SIGIL_OK) {
        fail(where, "the save changed on its way into the unit");
    } else {
        sigil_ps2_save_free(&expected);
    }
    char md5[33];
    sigil_md5_of(r->data, r->len, md5);
    if (strcmp(md5, r->content_hash) != 0) fail(where, "content hash is not the unit's md5");
    sigil_card_listing_free(l);
    sigil_ps2_card_free(&unit);
}

/* ---- checks ------------------------------------------------------------------- */

/* With shared cards, the default, the game's saves come off Mcd001.ps2 and
 * travel under the content's name. */
static void check_collect_shared(sigil_sync_result **kept) {
    size_t len = 0;
    uint8_t *card = shared_card(ORDER_A, 3, &len);
    if (!card) { fail("collect shared", "setup failed"); return; }
    mem_root root = {0};
    root_put(&root, "Mcd001.ps2", card, len);
    game g;
    make_game(&g, &root, "Ace Combat 04 (USA).iso", "SLUS-20152", false);
    sigil_sync_result *r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK) {
        fail("collect shared", "collect failed");
    } else {
        check_unit_is_ace_combat("collect shared", r);
        if (strcmp(r->artifact, "Ace Combat 04 (USA).ps2") != 0) fail("collect shared", "artifact name");
        if (!r->changed || !r->state) fail("collect shared", "a first collect is not a change, or no state");
        *kept = r;
        r = NULL;
    }
    sigil_sync_result_free(r);
    root_free(&root);
    free(card);
}

/* The unit is the same bytes wherever the save sat on the source card, and
 * the per-content card gives the same unit as the shared one. */
static void check_collect_deterministic(const sigil_sync_result *first) {
    size_t len = 0;
    uint8_t *card = shared_card(ORDER_B, 3, &len);
    if (!card || !first) { free(card); fail("collect deterministic", "setup failed"); return; }
    mem_root root = {0};
    root_put(&root, "Ace Combat 04 (USA).ps2", card, len);
    game g;
    make_game(&g, &root, "Ace Combat 04 (USA).iso", "SLUS-20152", true);
    sigil_sync_result *r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK) {
        fail("collect deterministic", "collect failed");
    } else {
        if (strcmp(r->content_hash, first->content_hash) != 0) fail("collect deterministic", "placement moved the content hash");
        if (strcmp(r->identity_hash, first->identity_hash) != 0) fail("collect deterministic", "placement moved the identity hash");
    }
    sigil_sync_result_free(r);
    root_free(&root);
    free(card);
}

/* A save whose times changed and whose data didn't is not a new save. */
static void check_times_are_not_a_change(const sigil_sync_result *first) {
    size_t len = 0;
    uint8_t *card = shared_card(ORDER_A, 3, &len);
    sigil_ps2_card c;
    sigil_ps2_save save;
    if (!card || !first || !load_card(card, len, &c)) { fail("times", "setup failed"); free(card); return; }
    sigil_card_listing *l = NULL;
    bool ok = sigil_ps2_card_list(&c, &l) == SIGIL_OK;
    for (size_t i = 0; ok && i < l->entry_count; i++) {
        if (strcmp(l->entries[i].name, ACE_COMBAT) != 0) continue;
        ok = sigil_ps2_extract(&c, l->entries[i].first_block, &save) == SIGIL_OK &&
             sigil_ps2_delete(&c, l->entries[i].first_block) == SIGIL_OK;
    }
    sigil_card_listing_free(l);
    if (ok) {
        save.entry[0x19] ^= 0x01;
        save.self[0x19] ^= 0x01;
        for (size_t i = 0; i < save.file_count; i++) {
            save.files[i].entry[0x0A] ^= 0x01;
            save.files[i].entry[0x1A] ^= 0x01;
        }
        ok = sigil_ps2_inject(&c, &save) == SIGIL_OK;
        sigil_ps2_save_free(&save);
    }
    free(card);
    card = ok ? card_bytes(&c, &len) : NULL;
    sigil_ps2_card_free(&c);
    if (!card) { fail("times", "setup failed"); return; }

    mem_root root = {0};
    root_put(&root, "Mcd001.ps2", card, len);
    game g;
    make_game(&g, &root, "Ace Combat 04 (USA).iso", "SLUS-20152", false);
    g.req.state = first->state;
    g.req.state_len = first->state_len;
    sigil_sync_result *r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK) fail("times", "collect failed");
    else if (r->changed || strcmp(r->identity_hash, first->identity_hash) != 0) fail("times", "new times read as a new save");
    sigil_sync_result_free(r);
    root_free(&root);
    free(card);
}

/* The unit with one byte of save `name`'s largest file flipped. */
static uint8_t *newer_unit_of(const uint8_t *data, size_t data_len, const char *name, size_t *len) {
    sigil_ps2_card unit;
    if (!load_card(data, data_len, &unit)) return NULL;
    sigil_ps2_save save;
    uint8_t *out = NULL;
    if (extract_named(&unit, name, &save)) {
        size_t biggest = 0;
        for (size_t i = 1; i < save.file_count; i++) {
            if (sigil_read_le32(save.files[i].entry + 4) > sigil_read_le32(save.files[biggest].entry + 4)) biggest = i;
        }
        save.files[biggest].data[100] ^= 0xFF;
        sigil_ps2_card fresh;
        if (sigil_ps2_card_format(&fresh, (const uint8_t[PS2_TOD_SIZE]){ 0, 0, 0, 0, 1, 1, 0xD0, 0x07 }) == SIGIL_OK) {
            if (sigil_ps2_inject(&fresh, &save) == SIGIL_OK) out = card_bytes(&fresh, len);
            sigil_ps2_card_free(&fresh);
        }
        sigil_ps2_save_free(&save);
    }
    sigil_ps2_card_free(&unit);
    return out;
}

static uint8_t *newer_unit(const sigil_sync_result *first, size_t *len) {
    return newer_unit_of(first->data, first->len, ACE_COMBAT, len);
}

/* Restoring a newer save of one game rewrites only that save: the other
 * games' saves and the system folder read back unchanged. Restoring over
 * saves never synced here is refused until the user says to overwrite. */
static void check_restore_keeps_other_games(const sigil_sync_result *first) {
    size_t len = 0, newer_len = 0;
    uint8_t *card = shared_card(ORDER_A, 3, &len);
    uint8_t *newer = first ? newer_unit(first, &newer_len) : NULL;
    sigil_ps2_card before;
    if (!card || !newer || !load_card(card, len, &before)) {
        fail("restore other games", "setup failed");
        free(card);
        free(newer);
        return;
    }
    mem_root root = {0};
    root_put(&root, "Mcd001.ps2", card, len);
    game g;
    make_game(&g, &root, "Ace Combat 04 (USA).iso", "SLUS-20152", false);

    sigil_sync_result *r = NULL;
    int rc = sigil_restore(&g.req, newer, newer_len, &r);
    if (rc != SIGIL_ERR_CONFLICT || !r || !r->conflict || root.writes != 0) {
        fail("restore other games", "saves never synced here were overwritten without asking");
    }
    sigil_sync_result_free(r);

    g.req.state = first->state;
    g.req.state_len = first->state_len;
    r = NULL;
    if (sigil_restore(&g.req, newer, newer_len, &r) != SIGIL_OK || root.writes != 1) {
        fail("restore other games", "restore of an unchanged game failed");
    } else {
        mem_file *f = root_find(&root, "Mcd001.ps2");
        sigil_ps2_card after;
        if (f->len != len || !load_card(f->data, f->len, &after)) {
            fail("restore other games", "the card changed size or no longer reads");
        } else {
            static const char *const OTHERS[] = { ATHF, NORRATH, REZ, DATA_SYSTEM };
            for (size_t i = 0; i < 4; i++) {
                sigil_ps2_save kept;
                if (!extract_named(&before, OTHERS[i], &kept)) { fail("restore other games", "setup lost a save"); continue; }
                if (sigil_ps2_verify(&after, &kept) != SIGIL_OK) fail("restore other games", "another save changed");
                sigil_ps2_save_free(&kept);
            }
            sigil_ps2_card newer_card;
            sigil_ps2_save wanted;
            if (load_card(newer, newer_len, &newer_card) && extract_named(&newer_card, ACE_COMBAT, &wanted)) {
                if (sigil_ps2_verify(&after, &wanted) != SIGIL_OK) fail("restore other games", "the newer save isn't on the card");
                sigil_ps2_save_free(&wanted);
                sigil_ps2_card_free(&newer_card);
            }
            sigil_ps2_card_free(&after);
        }
        refresh_listing(&g, &root);
        g.req.state = r->state;
        g.req.state_len = r->state_len;
        sigil_sync_result *back = NULL;
        if (sigil_collect(&g.req, &back) != SIGIL_OK || back->changed) fail("restore other games", "the restored save reads as a local change");
        sigil_sync_result_free(back);
    }
    sigil_sync_result_free(r);
    sigil_ps2_card_free(&before);
    root_free(&root);
    free(card);
    free(newer);
}

/* A card file sigil can't read is damaged, never formatted over: collect and
 * restore refuse naming it, with or without repair, since sigil can't rebuild
 * saves it can't read. An empty file holds nothing and takes the restore. */
static void check_unreadable_card(const sigil_sync_result *first) {
    size_t len = 0, newer_len = 0;
    uint8_t *card = shared_card(ORDER_A, 3, &len);
    uint8_t *newer = first ? newer_unit(first, &newer_len) : NULL;
    if (!card || !newer) { fail("unreadable card", "setup failed"); free(card); free(newer); return; }
    static const char *const WHAT[] = {
        "collect read a card with a bad magic as one without saves",
        "collect read a short card as one without saves",
        "collect read a card whose root won't list as one without saves",
    };
    for (int variant = 0; variant < 3; variant++) {
        uint8_t *bad = (uint8_t *)malloc(len);
        memcpy(bad, card, len);
        size_t bad_len = len;
        if (variant == 0) bad[0] ^= 0xFF;
        else if (variant == 1) bad_len = len - 528;
        else sigil_write_le32(bad + 0x50, 0x7FFFFFFFu);
        for (int repair = 0; repair < 2; repair++) {
            mem_root root = {0};
            root_put(&root, "Mcd001.ps2", bad, bad_len);
            game g;
            make_game(&g, &root, "Ace Combat 04 (USA).iso", "SLUS-20152", false);
            g.req.state = first->state;
            g.req.state_len = first->state_len;
            g.req.overwrite_local = 1;
            g.req.repair = repair;
            sigil_sync_result *seen = NULL, *r = NULL;
            if (sigil_collect(&g.req, &seen) != SIGIL_ERR_DAMAGED || !seen || strcmp(seen->problem, "Mcd001.ps2") != 0) {
                fail("unreadable card", WHAT[variant]);
            }
            if (sigil_restore(&g.req, newer, newer_len, &r) != SIGIL_ERR_DAMAGED || root.writes != 0 ||
                memcmp(root_find(&root, "Mcd001.ps2")->data, bad, bad_len) != 0) {
                fail("unreadable card", repair ? "restore with repair formatted over a card it can't read"
                                               : "restore formatted over a card it can't read");
            }
            sigil_sync_result_free(r);
            sigil_sync_result_free(seen);
            root_free(&root);
        }
        free(bad);
    }

    mem_root root = {0};
    root_put(&root, "Mcd001.ps2", card, 0);
    game g;
    make_game(&g, &root, "Ace Combat 04 (USA).iso", "SLUS-20152", false);
    sigil_sync_result *r = NULL;
    if (sigil_restore(&g.req, newer, newer_len, &r) != SIGIL_OK || root.writes != 1) {
        fail("unreadable card", "an empty card file didn't take the restore");
    }
    sigil_sync_result_free(r);
    root_free(&root);
    free(card);
    free(newer);
}

/* A first restore into an empty root makes the shared card PCSX2 would make,
 * and collects back to the same saves. */
static void check_restore_into_empty_root(const sigil_sync_result *first) {
    if (!first) return;
    mem_root root = {0};
    game g;
    make_game(&g, &root, "Ace Combat 04 (USA).iso", "SLUS-20152", false);
    sigil_sync_result *r = NULL;
    if (sigil_restore(&g.req, first->data, first->len, &r) != SIGIL_OK) {
        fail("restore empty root", "restore failed");
    } else if (!root_find(&root, "Mcd001.ps2")) {
        fail("restore empty root", "the card didn't go where the core keeps it");
    } else {
        refresh_listing(&g, &root);
        g.req.state = r->state;
        g.req.state_len = r->state_len;
        sigil_sync_result *back = NULL;
        if (sigil_collect(&g.req, &back) != SIGIL_OK) {
            fail("restore empty root", "collect after restore failed");
        } else {
            if (strcmp(back->identity_hash, first->identity_hash) != 0) fail("restore empty root", "saves differ after the trip");
            if (back->changed) fail("restore empty root", "a restored unit reads as a local change");
        }
        sigil_sync_result_free(back);
    }
    sigil_sync_result_free(r);
    root_free(&root);
}

/* A unit holding none of the game's saves is refused, and nothing is written. */
static void check_restore_foreign_unit(void) {
    size_t len = 0;
    uint8_t *mc01 = sample("mymc-mc01", &len);
    if (!mc01) return;
    mem_root root = {0};
    game g;
    make_game(&g, &root, "Ace Combat 04 (USA).iso", "SLUS-20152", false);
    sigil_sync_result *r = NULL;
    if (sigil_restore(&g.req, mc01, len, &r) != SIGIL_ERR_NOT_FOUND || root.writes != 0) {
        fail("restore foreign unit", "another game's unit was taken for this game");
    }
    sigil_sync_result_free(r);
    root_free(&root);
    free(mc01);
}

/* ---- PCSX2 folder cards ---------------------------------------------------------- */

#define CARD1 "memcards/Mcd001.ps2"
#define CARD2 "memcards/Mcd002.ps2"

static void make_standalone(game *g, mem_root *root, const char *content, const char *title_id) {
    make_game(g, root, content, title_id, false);
    g->req.save.layout = "pcsx2_standalone";
    g->req.remove = root_remove;
}

/* A folder-card sample's files as they sit under `card`. */
static bool put_sample_folder(mem_root *root, const char *card, const char *id) {
    size_t n = 0;
    for (size_t r = 0; r < g_manifest.nrows; r++) {
        if (strcmp(corpus_get(&g_manifest, r, "id"), id) != 0) continue;
        const char *rel = corpus_get(&g_manifest, r, "path");
        char at[SIGIL_SAVE_PATH_MAX];
        size_t len = 0;
        uint8_t *data = corpus_sample(&g_manifest, "ps2", id, rel, &len);
        if (!data) return false;
        snprintf(at, sizeof(at), "%s/%s", card, rel);
        root_put(root, at, data, len);
        free(data);
        n++;
    }
    return n > 0;
}

/* A save's files as PCSX2 keeps them, under `card`. */
static bool put_save_folder(mem_root *root, const char *card, const sigil_ps2_save *save) {
    sigil_ps2_folder_file *files = NULL;
    size_t n = 0;
    if (sigil_ps2_unpack(save, &files, &n) != SIGIL_OK) return false;
    char name[33];
    snprintf(name, sizeof(name), "%.32s", (const char *)save->entry + 0x40);
    for (size_t i = 0; i < n; i++) {
        char at[SIGIL_SAVE_PATH_MAX];
        snprintf(at, sizeof(at), "%s/%s/%s", card, name, files[i].path);
        root_put(root, at, files[i].data, files[i].len);
    }
    sigil_ps2_folder_files_free(files, n);
    return true;
}

static void put_superblock(mem_root *root, const char *card) {
    uint8_t sb[PS2_FOLDER_SUPERBLOCK_SIZE];
    sigil_ps2_folder_superblock(sb);
    char at[SIGIL_SAVE_PATH_MAX];
    snprintf(at, sizeof(at), "%s/_pcsx2_superblock", card);
    root_put(root, at, sb, sizeof(sb));
}

/* Every file `save` unpacks to sits under `card` with the same bytes. */
static bool folder_holds(mem_root *root, const char *card, const sigil_ps2_save *save) {
    sigil_ps2_folder_file *files = NULL;
    size_t n = 0;
    if (sigil_ps2_unpack(save, &files, &n) != SIGIL_OK) return false;
    char name[33];
    snprintf(name, sizeof(name), "%.32s", (const char *)save->entry + 0x40);
    bool ok = true;
    for (size_t i = 0; i < n && ok; i++) {
        char at[SIGIL_SAVE_PATH_MAX];
        snprintf(at, sizeof(at), "%s/%s/%s", card, name, files[i].path);
        mem_file *f = root_find(root, at);
        ok = f && f->len == files[i].len && memcmp(f->data, files[i].data, f->len) == 0;
    }
    sigil_ps2_folder_files_free(files, n);
    return ok;
}

static size_t files_under(mem_root *root, const char *prefix) {
    size_t n = 0;
    for (size_t i = 0; i < root->count; i++) n += strncmp(root->files[i].path, prefix, strlen(prefix)) == 0;
    return n;
}

/* PCSX2's folder card: the game's save folders pack into the same unit a
 * file card gives, and other games' folders stay out. */
static void check_folder_collect(const sigil_sync_result *first) {
    mem_root root = {0};
    put_superblock(&root, CARD1);
    if (!first || !put_sample_folder(&root, CARD1, "ace-combat-04-aethersx2") ||
        !put_sample_folder(&root, CARD1, "7-wonders-armsx2")) {
        fail("folder collect", "setup failed");
        root_free(&root);
        return;
    }
    game g;
    make_standalone(&g, &root, "Ace Combat 04 (USA).iso", "SLUS-20152");
    sigil_sync_result *r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK) {
        fail("folder collect", "collect failed");
    } else {
        check_unit_is_ace_combat("folder collect", r);
        if (strcmp(r->artifact, "Ace Combat 04 (USA).ps2") != 0) fail("folder collect", "artifact name");
        if (strcmp(r->identity_hash, first->identity_hash) != 0 || strcmp(r->content_hash, first->content_hash) != 0) {
            fail("folder collect", "the folder card gives another unit than the file card");
        }
    }
    sigil_sync_result_free(r);
    root_free(&root);
}

/* Restore unpacks the unit into the game's folder, leaves other games'
 * folders alone, and collects back to the same saves. A card that holds
 * saves behind an empty superblock is damaged: restore names it and writes
 * nothing until the request says to repair, then writes a full superblock.
 * A flow-style ARMSX2 index makes the same trip. */
static void check_folder_restore(const sigil_sync_result *first) {
    mem_root root = {0};
    if (!first || !put_sample_folder(&root, CARD1, "7-wonders-armsx2")) {
        fail("folder restore", "setup failed");
        root_free(&root);
        return;
    }
    root_put(&root, CARD1 "/_pcsx2_superblock", (const uint8_t *)"", 0);
    mem_root before = {0};
    put_sample_folder(&before, CARD1, "7-wonders-armsx2");
    game g;
    make_standalone(&g, &root, "Ace Combat 04 (USA).iso", "SLUS-20152");
    sigil_sync_result *r = NULL, *back = NULL;
    sigil_ps2_save expected;
    bool have_expected = folder_save("ace-combat-04-aethersx2", &expected);
    if (sigil_restore(&g.req, first->data, first->len, &r) != SIGIL_ERR_DAMAGED || root.writes != 0 || !r ||
        strcmp(r->problem, CARD1 "/_pcsx2_superblock") != 0) {
        fail("folder restore", "an empty superblock on a card with saves wasn't reported as damaged");
    }
    sigil_sync_result_free(r);
    r = NULL;
    g.req.repair = 1;
    if (sigil_restore(&g.req, first->data, first->len, &r) != SIGIL_OK) {
        fail("folder restore", "restore failed");
    } else {
        if (!have_expected || !folder_holds(&root, CARD1, &expected)) fail("folder restore", "the save's files aren't in its folder");
        mem_file *sb = root_find(&root, CARD1 "/_pcsx2_superblock");
        if (!sb || !sigil_ps2_folder_superblock_usable(sb->data, sb->len)) fail("folder restore", "the superblock is still unusable");
        if (root_find(&root, CARD1)) fail("folder restore", "a file card was made beside the folder card");
        for (size_t i = 0; i < before.count; i++) {
            mem_file *f = root_find(&root, before.files[i].path);
            if (!f || f->len != before.files[i].len || memcmp(f->data, before.files[i].data, f->len) != 0) {
                fail("folder restore", "another game's file changed");
            }
        }
        refresh_listing(&g, &root);
        g.req.state = r->state;
        g.req.state_len = r->state_len;
        if (sigil_collect(&g.req, &back) != SIGIL_OK || strcmp(back->identity_hash, first->identity_hash) != 0 || back->changed) {
            fail("folder restore", "saves differ after the trip");
        }
    }
    if (have_expected) sigil_ps2_save_free(&expected);
    sigil_sync_result_free(back);
    sigil_sync_result_free(r);
    root_free(&before);

    game w;
    make_standalone(&w, &root, "7 Wonders of the Ancient World (USA).iso", "SLUS-21693");
    sigil_sync_result *wonders = NULL, *placed = NULL, *again = NULL;
    if (sigil_collect(&w.req, &wonders) != SIGIL_OK || !wonders->data) {
        fail("folder restore", "the flow-style index didn't collect");
    } else {
        mem_root fresh = {0};
        put_superblock(&fresh, CARD1);
        game f;
        make_standalone(&f, &fresh, "7 Wonders of the Ancient World (USA).iso", "SLUS-21693");
        if (sigil_restore(&f.req, wonders->data, wonders->len, &placed) != SIGIL_OK ||
            !root_find(&fresh, CARD1 "/BASLUS-21693/_pcsx2_index")) {
            fail("folder restore", "the 7 Wonders save didn't land in its folder");
        } else {
            refresh_listing(&f, &fresh);
            f.req.state = placed->state;
            f.req.state_len = placed->state_len;
            if (sigil_collect(&f.req, &again) != SIGIL_OK || strcmp(again->identity_hash, wonders->identity_hash) != 0) {
                fail("folder restore", "the 7 Wonders save differs after the trip");
            }
        }
        root_free(&fresh);
    }
    sigil_sync_result_free(again);
    sigil_sync_result_free(placed);
    sigil_sync_result_free(wonders);
    root_free(&root);
}

/* memcardFilters: a game whose saves carry another serial finds them through
 * game_ids, on a file card and on a folder card. */
static void check_memcard_filters(const sigil_sync_result *first) {
    if (!first) return;
    size_t len = 0;
    uint8_t *card = shared_card(ORDER_A, 3, &len);
    mem_root file = {0}, folder = {0};
    if (card) root_put(&file, "Mcd001.ps2", card, len);
    put_superblock(&folder, CARD1);
    put_sample_folder(&folder, CARD1, "ace-combat-04-aethersx2");
    for (int i = 0; i < 2; i++) {
        game g;
        if (i == 0) make_game(&g, &file, "Other Disc (USA).iso", "SLUS-99999", false);
        else make_standalone(&g, &folder, "Other Disc (USA).iso", "SLUS-99999");
        g.req.game_ids = ACE_IDS;
        g.req.game_id_count = 1;
        sigil_sync_result *r = NULL;
        if (sigil_collect(&g.req, &r) != SIGIL_OK || strcmp(r->identity_hash, first->identity_hash) != 0) {
            fail("memcard filters", i == 0 ? "the file card's save under another serial wasn't found"
                                          : "the folder card's save under another serial wasn't found");
        }
        sigil_sync_result_free(r);
    }
    root_free(&file);
    root_free(&folder);
    free(card);
}

/* Local saves already equal to the unit aren't a conflict even with no
 * state, and nothing is written. */
static void check_equal_restore(const sigil_sync_result *first) {
    size_t len = 0;
    uint8_t *card = first ? shared_card(ORDER_A, 3, &len) : NULL;
    if (!card) return;
    mem_root root = {0};
    root_put(&root, "Mcd001.ps2", card, len);
    game g;
    make_game(&g, &root, "Ace Combat 04 (USA).iso", "SLUS-20152", false);
    sigil_sync_result *r = NULL;
    if (sigil_restore(&g.req, first->data, first->len, &r) != SIGIL_OK || root.writes != 0) {
        fail("equal restore", "restoring the saves already there conflicted or wrote");
    }
    sigil_sync_result_free(r);
    root_free(&root);
    free(card);
}

/* A folder-card restore writes only the files whose bytes differ, writes a
 * new card's superblock before any save file, and on Mcd002 alone uses it
 * without making an Mcd001. */
static void check_folder_writes(const sigil_sync_result *first) {
    size_t newer_len = 0;
    uint8_t *newer = first ? newer_unit(first, &newer_len) : NULL;
    if (!newer) { fail("folder writes", "setup failed"); return; }
    for (int slot = 1; slot <= 2; slot++) {
        const char *card = slot == 1 ? CARD1 : CARD2;
        mem_root root = {0};
        char sb[SIGIL_SAVE_PATH_MAX];
        snprintf(sb, sizeof(sb), "%s/_pcsx2_superblock", card);
        root_put(&root, sb, (const uint8_t *)"", 0);
        game g;
        make_standalone(&g, &root, "Ace Combat 04 (USA).iso", "SLUS-20152");
        sigil_sync_result *r = NULL, *again = NULL;
        if (sigil_restore(&g.req, first->data, first->len, &r) != SIGIL_OK) {
            fail("folder writes", "restore into a new card failed");
        } else {
            if (strcmp(root.first_write, sb) != 0) fail("folder writes", "a save file was written before the superblock");
            if (root_find(&root, slot == 1 ? CARD2 "/_pcsx2_superblock" : CARD1)) fail("folder writes", "the other slot was made");
            refresh_listing(&g, &root);
            g.req.state = r->state;
            g.req.state_len = r->state_len;
            int writes = root.writes;
            if (sigil_restore(&g.req, newer, newer_len, &again) != SIGIL_OK || root.writes - writes != 1) {
                fail("folder writes", "a newer save with one changed file didn't write exactly that file");
            }
        }
        sigil_sync_result_free(again);
        sigil_sync_result_free(r);
        root_free(&root);
    }
    free(newer);
}

/* Slot 1 a file card that would take the newer save, slot 2 a folder card
 * holding a save of the game the unit lacks: without a remove callback the
 * restore refuses before writing either card. */
static void check_refusal_spans_slots(void) {
    size_t mc01_len = 0;
    uint8_t *mc01 = sample("mymc-mc01", &mc01_len);
    sigil_ps2_card card;
    sigil_ps2_save ace, xx;
    if (!mc01 || !load_card(mc01, mc01_len, &card)) { free(mc01); fail("refusal spans slots", "setup failed"); return; }
    bool ok = folder_save("ace-combat-04-aethersx2", &ace) && folder_save("ace-combat-04-aethersx2", &xx);
    if (ok) {
        memset(xx.entry + 0x40, 0, 32);
        memcpy(xx.entry + 0x40, "BASLUS-20152XX", 14);
        ok = sigil_ps2_inject(&card, &xx) == SIGIL_OK;
    }
    size_t file_len = 0;
    uint8_t *file = ok ? card_bytes(&card, &file_len) : NULL;
    sigil_ps2_card unit_card;
    uint8_t *unit = NULL;
    size_t unit_len = 0;
    if (file && sigil_ps2_card_format(&unit_card, (const uint8_t[PS2_TOD_SIZE]){ 0, 0, 0, 0, 1, 1, 0xD0, 0x07 }) == SIGIL_OK) {
        xx.files[0].data[10] ^= 0xFF;
        if (sigil_ps2_inject(&unit_card, &xx) == SIGIL_OK) unit = card_bytes(&unit_card, &unit_len);
        sigil_ps2_card_free(&unit_card);
    }
    if (!unit) {
        fail("refusal spans slots", "setup failed");
    } else {
        mem_root root = {0};
        root_put(&root, CARD1, file, file_len);
        put_superblock(&root, CARD2);
        put_save_folder(&root, CARD2, &ace);
        game g;
        make_standalone(&g, &root, "Ace Combat 04 (USA).iso", "SLUS-20152");
        g.req.remove = NULL;
        g.req.overwrite_local = 1;
        sigil_sync_result *r = NULL;
        if (sigil_restore(&g.req, unit, unit_len, &r) != SIGIL_ERR_INVALID_ARG || root.writes != 0) {
            fail("refusal spans slots", "slot 1 was written before slot 2 refused");
        }
        sigil_sync_result_free(r);
        root_free(&root);
    }
    if (ok) { sigil_ps2_save_free(&ace); sigil_ps2_save_free(&xx); }
    sigil_ps2_card_free(&card);
    free(unit);
    free(file);
    free(mc01);
}

/* A companion's save folder on a folder card: collect returns it as the
 * companion's unit; a restore without the companion's unit leaves its files
 * alone, one with a newer unit rewrites them. */
static void check_folder_companion(const sigil_sync_result *first) {
    size_t newer_len = 0;
    uint8_t *newer = first ? newer_unit(first, &newer_len) : NULL;
    mem_root root = {0};
    put_superblock(&root, CARD1);
    bool ok = newer && put_sample_folder(&root, CARD1, "7-wonders-armsx2") &&
              put_sample_folder(&root, CARD1, "ace-combat-04-aethersx2");
    static const char *const ACE[] = { "SLUS-20152" };
    sigil_sync_companion companion = { ACE, 1, NULL, 0 };
    game g;
    make_standalone(&g, &root, "7 Wonders of the Ancient World (USA).iso", "SLUS-21693");
    g.req.companions = &companion;
    g.req.companion_count = 1;
    sigil_sync_result *wonders = NULL, *r = NULL, *again = NULL;
    if (!ok || sigil_collect(&g.req, &wonders) != SIGIL_OK || !wonders->data) {
        fail("folder companion", "setup failed");
    } else if (wonders->companion_count != 1 || strcmp(wonders->companions[0].identity_hash, first->identity_hash) != 0) {
        fail("folder companion", "collect didn't return the companion's folder as its unit");
    } else {
        g.req.state = wonders->state;
        g.req.state_len = wonders->state_len;
        mem_root before = {0};
        put_sample_folder(&before, CARD1, "ace-combat-04-aethersx2");
        size_t newer_wonders_len = 0;
        uint8_t *newer_wonders = newer_unit_of(wonders->data, wonders->len, "BASLUS-21693", &newer_wonders_len);
        int rc = newer_wonders ? sigil_restore(&g.req, newer_wonders, newer_wonders_len, &r) : SIGIL_ERR_OOM;
        free(newer_wonders);
        if (rc != SIGIL_OK) {
            fail("folder companion", "restore without the companion's unit failed");
        } else {
            for (size_t i = 0; i < before.count; i++) {
                mem_file *f = root_find(&root, before.files[i].path);
                if (!f || f->len != before.files[i].len || memcmp(f->data, before.files[i].data, f->len) != 0) {
                    fail("folder companion", "the companion's files changed without its unit");
                    break;
                }
            }
        }
        root_free(&before);
        refresh_listing(&g, &root);
        g.req.state = r ? r->state : NULL;
        g.req.state_len = r ? r->state_len : 0;
        companion.unit = newer;
        companion.unit_len = newer_len;
        sigil_ps2_card newer_card;
        sigil_ps2_save wanted;
        if (sigil_restore(&g.req, wonders->data, wonders->len, &again) != SIGIL_OK) {
            fail("folder companion", "restore with the companion's newer unit failed");
        } else if (load_card(newer, newer_len, &newer_card)) {
            if (extract_named(&newer_card, ACE_COMBAT, &wanted)) {
                if (!folder_holds(&root, CARD1, &wanted)) fail("folder companion", "the companion's folder didn't take its newer unit");
                sigil_ps2_save_free(&wanted);
            }
            sigil_ps2_card_free(&newer_card);
        }
    }
    sigil_sync_result_free(again);
    sigil_sync_result_free(r);
    sigil_sync_result_free(wonders);
    root_free(&root);
    free(newer);
}

/* A save whose file mode PCSX2 couldn't rebuild from the index goes onto a
 * folder card with its _pcsx2_meta, and collects back with the same mode. */
static void check_folder_keeps_modes(void) {
    sigil_ps2_save save;
    if (!folder_save("ace-combat-04-aethersx2", &save) || save.file_count == 0) { fail("folder modes", "setup failed"); return; }
    save.files[0].entry[1] ^= 0x20;
    sigil_ps2_card unit_card;
    uint8_t *unit = NULL;
    size_t unit_len = 0;
    if (sigil_ps2_card_format(&unit_card, (const uint8_t[PS2_TOD_SIZE]){ 0, 0, 0, 0, 1, 1, 0xD0, 0x07 }) == SIGIL_OK) {
        if (sigil_ps2_inject(&unit_card, &save) == SIGIL_OK) unit = card_bytes(&unit_card, &unit_len);
        sigil_ps2_card_free(&unit_card);
    }
    mem_root root = {0};
    put_superblock(&root, CARD1);
    game g;
    make_standalone(&g, &root, "Ace Combat 04 (USA).iso", "SLUS-20152");
    sigil_sync_result *r = NULL, *back = NULL;
    if (!unit || sigil_restore(&g.req, unit, unit_len, &r) != SIGIL_OK) {
        fail("folder modes", "restore failed");
    } else {
        refresh_listing(&g, &root);
        sigil_ps2_card card;
        sigil_ps2_save got;
        if (sigil_collect(&g.req, &back) != SIGIL_OK || !back->data || !load_card(back->data, back->len, &card)) {
            fail("folder modes", "collect failed");
        } else {
            char name[33];
            snprintf(name, sizeof(name), "%.32s", (const char *)save.entry + 0x40);
            if (!extract_named(&card, name, &got)) {
                fail("folder modes", "the save didn't come back");
            } else {
                if (got.file_count != save.file_count || memcmp(got.files[0].entry, save.files[0].entry, 4) != 0) {
                    fail("folder modes", "a file's mode was lost on the folder card");
                }
                sigil_ps2_save_free(&got);
            }
            sigil_ps2_card_free(&card);
        }
    }
    sigil_sync_result_free(back);
    sigil_sync_result_free(r);
    root_free(&root);
    free(unit);
    sigil_ps2_save_free(&save);
}

static bool put_big_foreign_folder(mem_root *root, const char *name, size_t size);

/* PCSX2 shows the game the system folders too, so they count against the
 * card's space. */
static void check_system_folder_counts(const sigil_sync_result *first) {
    static const char *const SYSTEM[] = { "BEDATA-SYSTEM", "BWNETCNF" };
    for (size_t i = 0; i < 2; i++) {
        mem_root root = {0};
        put_superblock(&root, CARD1);
        if (!first || !put_big_foreign_folder(&root, SYSTEM[i], 8250000)) {
            fail("system folder space", "setup failed");
            root_free(&root);
            return;
        }
        game g;
        make_standalone(&g, &root, "Ace Combat 04 (USA).iso", "SLUS-20152");
        sigil_sync_result *r = NULL;
        if (sigil_restore(&g.req, first->data, first->len, &r) != SIGIL_ERR_NO_SPACE || root.writes != 0) {
            fail("system folder space", SYSTEM[i]);
        }
        sigil_sync_result_free(r);
        root_free(&root);
    }
}

/* Where the unit card holds the first bytes of its save's largest file; a
 * restore into an empty root writes the same image. */
static size_t largest_file_offset(const sigil_sync_result *unit) {
    sigil_ps2_card card;
    sigil_ps2_save save;
    size_t at = 0;
    if (!load_card(unit->data, unit->len, &card)) return 0;
    if (extract_named(&card, ACE_COMBAT, &save)) {
        size_t biggest = 0;
        for (size_t i = 1; i < save.file_count; i++) {
            if (sigil_read_le32(save.files[i].entry + 4) > sigil_read_le32(save.files[biggest].entry + 4)) biggest = i;
        }
        for (size_t i = 0; i + 64 <= unit->len && !at; i++) {
            if (memcmp(unit->data + i, save.files[biggest].data, 64) == 0) at = i + 10;
        }
        sigil_ps2_save_free(&save);
    }
    sigil_ps2_card_free(&card);
    return at;
}

/* A write that fails or reads back different is an I/O error, on a file
 * card and on a folder card. */
static void check_faulty_writes(const sigil_sync_result *first) {
    if (!first) return;
    size_t data_at = largest_file_offset(first);
    if (!data_at) { fail("faulty writes", "setup failed"); return; }
    for (int folder = 0; folder < 2; folder++) {
        for (int corrupt = 0; corrupt < 2; corrupt++) {
            mem_root root = {0};
            if (folder) root_put(&root, CARD1 "/_pcsx2_superblock", (const uint8_t *)"", 0);
            int target = folder ? 2 : 1;
            if (corrupt) {
                root.corrupt_write = target;
                root.corrupt_at = folder ? 0 : data_at;
            } else {
                root.fail_write = target;
            }
            game g;
            if (folder) make_standalone(&g, &root, "Ace Combat 04 (USA).iso", "SLUS-20152");
            else make_game(&g, &root, "Ace Combat 04 (USA).iso", "SLUS-20152", false);
            sigil_sync_result *r = NULL;
            if (sigil_restore(&g.req, first->data, first->len, &r) != SIGIL_ERR_IO) {
                fail("faulty writes", folder ? (corrupt ? "a folder file read back corrupted" : "a folder write failed")
                                             : (corrupt ? "a file card read back corrupted" : "a file card write failed"));
            }
            sigil_sync_result_free(r);
            root_free(&root);
        }
    }
}

/* A folder card with no saves and no usable superblock is new: restore
 * formats it without being asked. A save folder whose _pcsx2_index doesn't
 * parse is damaged: collect and restore name the index and change nothing;
 * with repair, collect packs the folder without it, and restore counts it as
 * the game's local save (so the conflict rule guards it) and writes a fresh
 * index. */
/* A save folder of the game that sigil can't pack (a subdirectory, a file
 * name longer than a card entry holds) still shows in PCSX2, so it is
 * damaged rather than missing: collect and restore refuse naming it, with or
 * without repair, and restore removes none of its files. */
static void check_unpackable_folder(const sigil_sync_result *first) {
    if (!first) return;
    /* The game's folder whose name also matches PCSX2's system filter is
     * still the game's; a real system folder that won't pack is left off. */
    static const struct { const char *extra, *problem; } CASES[] = {
        { CARD1 "/" ACE_COMBAT "/sub/x", CARD1 "/" ACE_COMBAT },
        { CARD1 "/" ACE_COMBAT "/a-file-name-longer-than-a-card-entry-holds", CARD1 "/" ACE_COMBAT },
        { CARD1 "/BASLUS-20152DATA-SYSTEM/sub/x", CARD1 "/BASLUS-20152DATA-SYSTEM" },
        { CARD1 "/" DATA_SYSTEM "/sub/x", NULL },
    };
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        for (int repair = 0; repair < 2; repair++) {
            mem_root root = {0};
            put_superblock(&root, CARD1);
            put_sample_folder(&root, CARD1, "ace-combat-04-aethersx2");
            root_put(&root, CASES[i].extra, (const uint8_t *)"x", 1);
            game g;
            make_standalone(&g, &root, "Ace Combat 04 (USA).iso", "SLUS-20152");
            g.req.remove = root_remove;
            g.req.repair = repair;
            g.req.overwrite_local = 1;
            sigil_sync_result *seen = NULL, *r = NULL;
            int collected = sigil_collect(&g.req, &seen);
            if (!CASES[i].problem) {
                if (collected != SIGIL_OK) fail("unpackable folder", "a system folder sigil can't pack stopped the collect");
                sigil_sync_result_free(seen);
                root_free(&root);
                continue;
            }
            if (collected != SIGIL_ERR_DAMAGED || !seen || strcmp(seen->problem, CASES[i].problem) != 0) {
                fail("unpackable folder", "collect read a save folder it can't pack as missing");
            }
            if (sigil_restore(&g.req, first->data, first->len, &r) != SIGIL_ERR_DAMAGED || root.writes != 0 ||
                root.removes != 0) {
                fail("unpackable folder", "restore touched a save folder it can't pack");
            }
            sigil_sync_result_free(r);
            sigil_sync_result_free(seen);
            root_free(&root);
        }
    }
}

/* PCSX2 reads all 0x2000 bytes of _pcsx2_superblock: a shorter one marked
 * formatted still hides the card's saves, so it is damaged; a new superblock
 * that reads back wrong fails the restore. */
static void check_superblock_rules(const sigil_sync_result *first) {
    if (!first) return;
    uint8_t sb[PS2_FOLDER_SUPERBLOCK_SIZE];
    sigil_ps2_folder_superblock(sb);
    mem_root root = {0};
    root_put(&root, CARD1 "/_pcsx2_superblock", sb, 0x100);
    put_sample_folder(&root, CARD1, "ace-combat-04-aethersx2");
    game g;
    make_standalone(&g, &root, "Ace Combat 04 (USA).iso", "SLUS-20152");
    g.req.overwrite_local = 1;
    sigil_sync_result *r = NULL;
    size_t newer_len = 0;
    uint8_t *newer = newer_unit(first, &newer_len);
    if (!newer || sigil_restore(&g.req, newer, newer_len, &r) != SIGIL_ERR_DAMAGED || root.writes != 0) {
        fail("superblock rules", "a short superblock marked formatted read as usable");
    }
    free(newer);
    sigil_sync_result_free(r);
    r = NULL;
    root_free(&root);

    root_put(&root, CARD1 "/_pcsx2_superblock", sb, 0);
    root.corrupt_write = 1;
    root.corrupt_at = 0x16;
    make_standalone(&g, &root, "Ace Combat 04 (USA).iso", "SLUS-20152");
    if (sigil_restore(&g.req, first->data, first->len, &r) != SIGIL_ERR_IO || strcmp(root.first_write, CARD1 "/_pcsx2_superblock") != 0) {
        fail("superblock rules", "a new superblock that read back wrong passed the restore");
    }
    sigil_sync_result_free(r);
    root_free(&root);
}

static void check_folder_damage(const sigil_sync_result *first) {
    if (!first) return;
    mem_root fresh = {0};
    root_put(&fresh, CARD1 "/_pcsx2_superblock", (const uint8_t *)"", 0);
    game n;
    make_standalone(&n, &fresh, "Ace Combat 04 (USA).iso", "SLUS-20152");
    sigil_sync_result *r = NULL;
    mem_file *sb = NULL;
    if (sigil_restore(&n.req, first->data, first->len, &r) != SIGIL_OK ||
        !(sb = root_find(&fresh, CARD1 "/_pcsx2_superblock")) || !sigil_ps2_folder_superblock_usable(sb->data, sb->len)) {
        fail("folder damage", "a new folder card wasn't formatted on restore");
    }
    sigil_sync_result_free(r);
    root_free(&fresh);

    static const char *const INDEX = CARD1 "/" ACE_COMBAT "/_pcsx2_index";
    mem_root root = {0};
    put_superblock(&root, CARD1);
    put_sample_folder(&root, CARD1, "ace-combat-04-aethersx2");
    root_put(&root, INDEX, (const uint8_t *)"{{{ not yaml", 12);
    game g;
    make_standalone(&g, &root, "Ace Combat 04 (USA).iso", "SLUS-20152");
    r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_ERR_DAMAGED || !r || strcmp(r->problem, INDEX) != 0) {
        fail("folder damage", "collect didn't name the unparseable index");
    }
    sigil_sync_result_free(r);
    r = NULL;
    if (sigil_restore(&g.req, first->data, first->len, &r) != SIGIL_ERR_DAMAGED || root.writes != 0 || !r ||
        strcmp(r->problem, INDEX) != 0) {
        fail("folder damage", "restore over an unparseable index wrote or didn't name it");
    }
    sigil_sync_result_free(r);
    r = NULL;

    g.req.repair = 1;
    if (sigil_collect(&g.req, &r) != SIGIL_OK || !r->data) {
        fail("folder damage", "collect with repair didn't pack the folder");
    } else {
        sigil_ps2_card unit;
        sigil_ps2_save save;
        if (!load_card(r->data, r->len, &unit) || !extract_named(&unit, ACE_COMBAT, &save)) {
            fail("folder damage", "the repaired unit doesn't hold the save");
        } else {
            sigil_ps2_save_free(&save);
            sigil_ps2_card_free(&unit);
        }
    }
    sigil_sync_result_free(r);
    r = NULL;
    if (sigil_restore(&g.req, first->data, first->len, &r) != SIGIL_ERR_CONFLICT || root.writes != 0) {
        fail("folder damage", "with repair, the damaged folder wasn't guarded as the game's local save");
    }
    sigil_sync_result_free(r);
    r = NULL;
    g.req.overwrite_local = 1;
    sigil_ps2_save expected;
    if (sigil_restore(&g.req, first->data, first->len, &r) != SIGIL_OK || !folder_save("ace-combat-04-aethersx2", &expected)) {
        fail("folder damage", "restore with repair and overwrite failed");
    } else {
        if (!folder_holds(&root, CARD1, &expected)) fail("folder damage", "the folder wasn't rewritten with a fresh index");
        sigil_ps2_save_free(&expected);
    }
    sigil_sync_result_free(r);
    root_free(&root);
}

/* A restore removes the files of the game's folders the unit lacks, keeps a
 * usable superblock and the system folder as they are, and without a remove
 * callback refuses before writing. */
static void check_folder_removes(const sigil_sync_result *first) {
    size_t mc01_len = 0;
    uint8_t *mc01 = sample("mymc-mc01", &mc01_len);
    sigil_ps2_card mc01_card;
    sigil_ps2_save system;
    if (!first || !mc01 || !load_card(mc01, mc01_len, &mc01_card)) { free(mc01); fail("folder removes", "setup failed"); return; }
    bool have_system = extract_named(&mc01_card, DATA_SYSTEM, &system);
    sigil_ps2_card_free(&mc01_card);
    free(mc01);
    if (!have_system) { fail("folder removes", "setup failed"); return; }

    for (int with_remove = 1; with_remove >= 0; with_remove--) {
        mem_root root = {0};
        uint8_t sb[PS2_FOLDER_SUPERBLOCK_SIZE];
        sigil_ps2_folder_superblock(sb);
        sb[PS2_FOLDER_SUPERBLOCK_SIZE - 1] = 0x5A;
        root_put(&root, CARD1 "/_pcsx2_superblock", sb, sizeof(sb));
        put_sample_folder(&root, CARD1, "ace-combat-04-aethersx2");
        root_put(&root, CARD1 "/" ACE_COMBAT "/stale.bin", (const uint8_t *)"old", 3);
        sigil_ps2_save ace;
        if (folder_save("ace-combat-04-aethersx2", &ace)) {
            memcpy(ace.entry + 0x40, "BASLUS-20152XX\0", 15);
            put_save_folder(&root, CARD1, &ace);
            sigil_ps2_save_free(&ace);
        }
        put_save_folder(&root, CARD1, &system);
        size_t system_files = files_under(&root, CARD1 "/" DATA_SYSTEM "/");

        game g;
        make_standalone(&g, &root, "Ace Combat 04 (USA).iso", "SLUS-20152");
        g.req.overwrite_local = 1;
        if (!with_remove) g.req.remove = NULL;
        sigil_sync_result *r = NULL;
        int rc = sigil_restore(&g.req, first->data, first->len, &r);
        if (!with_remove) {
            if (rc != SIGIL_ERR_INVALID_ARG || root.writes != 0) fail("folder removes", "removing ran without a remove callback");
        } else if (rc != SIGIL_OK) {
            fail("folder removes", "restore failed");
        } else {
            if (root_find(&root, CARD1 "/" ACE_COMBAT "/stale.bin")) fail("folder removes", "a file the unit lacks is still there");
            if (files_under(&root, CARD1 "/BASLUS-20152XX/") != 0) fail("folder removes", "a save folder the unit lacks is still there");
            if (root.dirs_removed != 1) fail("folder removes", "a dropped save folder's directory stayed, which PCSX2 would show");
            mem_file *kept = root_find(&root, CARD1 "/_pcsx2_superblock");
            if (!kept || kept->len != sizeof(sb) || memcmp(kept->data, sb, sizeof(sb)) != 0) fail("folder removes", "a usable superblock was rewritten");
            if (!folder_holds(&root, CARD1, &system) || files_under(&root, CARD1 "/" DATA_SYSTEM "/") != system_files) {
                fail("folder removes", "the system folder changed");
            }
        }
        sigil_sync_result_free(r);
        root_free(&root);
    }
    sigil_ps2_save_free(&system);
}

/* The game's save on a file card that lists as corrupt (its folder entry
 * counts more entries than the card holds), or lists but won't extract (a
 * file entry whose mode isn't a file's), is DAMAGED naming the card on
 * collect and restore: reading on would take the save as deleted. */
/* The FAT entry of relative cluster `n`, found as the PS2 finds it: through
 * the superblock's indirect FAT clusters to the FAT cluster holding it. */
static uint8_t *fat_slot(sigil_ps2_card *card, uint32_t n) {
    uint32_t per = card->cluster_size / 4, fat_index = n / per;
    uint32_t indirect = card->ifc[fat_index / per];
    uint32_t fat_cluster = sigil_read_le32(card->clusters + (size_t)indirect * card->cluster_size + (fat_index % per) * 4);
    return card->clusters + (size_t)fat_cluster * card->cluster_size + (n % per) * 4;
}

/* Ends the chain of `save`'s first file longer than a cluster at its first
 * cluster, so the chain is shorter than the file's length says. */
static bool cut_first_long_chain(sigil_ps2_card *card, const sigil_ps2_save *save) {
    for (size_t i = 0; i < save->file_count; i++) {
        const uint8_t *e = save->files[i].entry;
        if (sigil_read_le32(e + 0x04) <= card->cluster_size) continue;
        uint8_t *slot = fat_slot(card, sigil_read_le32(e + 0x10));
        sigil_write_le32(slot, 0xFFFFFFFFu);
        if (card->dirty) card->dirty[(size_t)(slot - card->clusters) / 512] = 1;
        return true;
    }
    return false;
}

static void check_broken_own_save(const sigil_sync_result *first) {
    size_t mc01_len = 0, newer_len = 0;
    uint8_t *mc01 = sample("mymc-mc01", &mc01_len);
    uint8_t *newer = first ? newer_unit(first, &newer_len) : NULL;
    if (!mc01 || !newer) { fail("broken own save", "setup failed"); free(mc01); free(newer); return; }
    /* variant 2: another game's folder entry pointing at Ace Combat's
     * clusters, so both folders claim one chain. variant 3: a file's FAT
     * chain ending before its length. */
    static const char *const WHAT[] = { "a corrupt save read as missing", "a save that won't extract read as missing",
                                        "a save sharing its clusters with another read as missing",
                                        "a save whose chain is shorter than its file read as missing" };
    for (int variant = 0; variant < 4; variant++) {
        sigil_ps2_card card;
        sigil_ps2_save ace, athf;
        size_t len = 0;
        uint8_t *bytes = NULL;
        if (load_card(mc01, mc01_len, &card)) {
            if (folder_save("ace-combat-04-aethersx2", &ace)) {
                if (variant == 1) sigil_write_le16(ace.files[0].entry, 0x8427);
                bool ok = sigil_ps2_inject(&card, &ace) == SIGIL_OK;
                if (ok && variant == 2) {
                    ok = folder_save("athf-aethersx2", &athf) && sigil_ps2_inject(&card, &athf) == SIGIL_OK;
                    if (ok) sigil_ps2_save_free(&athf);
                }
                size_t total = (size_t)card.clusters_per_card * card.cluster_size;
                uint8_t *ace_entry = NULL, *athf_entry = NULL;
                for (size_t at = 0; ok && at + 512 <= total; at += 512) {
                    uint8_t *e = card.clusters + at;
                    if (!(sigil_read_le16(e) & 0x0020) || !(sigil_read_le16(e) & 0x8000)) continue;
                    if (strcmp((const char *)e + 0x40, ACE_COMBAT) == 0) ace_entry = e;
                    if (strcmp((const char *)e + 0x40, ATHF) == 0) athf_entry = e;
                }
                uint8_t *edited = variant == 0 ? ace_entry : variant == 2 ? athf_entry : NULL;
                if (edited && variant == 0) sigil_write_le32(edited + 4, 0x00FFFFFFu);
                if (edited && variant == 2 && ace_entry) memcpy(edited + 0x10, ace_entry + 0x10, 4);
                if (edited && card.dirty) card.dirty[(size_t)(edited - card.clusters) / 512] = 1;
                sigil_ps2_save placed;
                bool cut = false;
                if (ok && variant == 3 && extract_named(&card, ACE_COMBAT, &placed)) {
                    cut = cut_first_long_chain(&card, &placed);
                    sigil_ps2_save_free(&placed);
                }
                if (ok && (variant == 1 || edited || cut)) bytes = card_bytes(&card, &len);
                sigil_ps2_save_free(&ace);
            }
            sigil_ps2_card_free(&card);
        }
        sigil_ps2_card listed;
        sigil_card_listing *l = NULL;
        if (!bytes || !load_card(bytes, len, &listed)) { fail("broken own save", "setup failed"); free(bytes); continue; }
        int listed_rc = sigil_ps2_card_list(&listed, &l);
        if (listed_rc == SIGIL_OK && variant == 0 &&
            (l->corrupt_entry_count != 1 || strcmp(l->corrupt_entries[0].name, ACE_COMBAT) != 0 ||
             strcmp(l->corrupt_entries[0].owner_id, "SLUS-20152") != 0)) {
            fail("broken own save", "the corrupt save isn't named with its owner");
        }
        if (listed_rc == SIGIL_OK && variant == 2 && l->corrupt_entry_count != 2) {
            fail("broken own save", "two folders sharing clusters listed");
        }
        if (listed_rc == SIGIL_OK && variant == 3) {
            sigil_ps2_save gone;
            if (l->corrupt_entry_count != 1 || strcmp(l->corrupt_entries[0].name, ACE_COMBAT) != 0) {
                fail("broken own save", "a save whose chain is shorter than its file listed");
            } else if (sigil_ps2_extract(&listed, l->corrupt_entries[0].first_block, &gone) == SIGIL_OK) {
                sigil_ps2_save_free(&gone);
                fail("broken own save", "a save whose chain is shorter than its file extracted");
            }
        }
        sigil_card_listing_free(l);
        sigil_ps2_card_free(&listed);

        mem_root root = {0};
        root_put(&root, "Mcd001.ps2", bytes, len);
        game g;
        make_game(&g, &root, "Ace Combat 04 (USA).iso", "SLUS-20152", false);
        g.req.overwrite_local = 1;
        sigil_sync_result *seen = NULL, *r = NULL;
        if (sigil_collect(&g.req, &seen) != SIGIL_ERR_DAMAGED || !seen || strcmp(seen->problem, "Mcd001.ps2") != 0 ||
            sigil_restore(&g.req, newer, newer_len, &r) != SIGIL_ERR_DAMAGED || root.writes != 0) {
            fail("broken own save", WHAT[variant]);
        }
        sigil_sync_result_free(r);
        sigil_sync_result_free(seen);
        root_free(&root);
        free(bytes);
    }
    free(mc01);
    free(newer);
}

/* A unit's save folder names the directory restore writes under the card:
 * one that would leave it (a '/', ".", "..") is a corrupt save, refused
 * before any write. */
static void check_folder_names_stay_inside(void) {
    static const char *const NAMES[] = { "BASLUS-20152/../../../../x", "..", ".", "BASLUS-20152\\..\\x" };
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        sigil_ps2_card card;
        sigil_ps2_save ace;
        if (sigil_ps2_card_format(&card, (const uint8_t[8]){ 0, 0, 0, 0, 1, 1, 0xD0, 0x07 }) != SIGIL_OK) {
            fail("folder names", "setup failed");
            return;
        }
        size_t len = 0;
        uint8_t *unit = NULL;
        if (folder_save("ace-combat-04-aethersx2", &ace)) {
            memset(ace.entry + 0x40, 0, 32);
            memcpy(ace.entry + 0x40, NAMES[i], strlen(NAMES[i]));
            if (sigil_ps2_inject(&card, &ace) == SIGIL_OK) unit = card_bytes(&card, &len);
            sigil_ps2_save_free(&ace);
        }
        sigil_ps2_card_free(&card);
        if (!unit) { fail("folder names", "setup failed"); return; }

        mem_root root = {0};
        put_superblock(&root, CARD1);
        game g;
        make_standalone(&g, &root, "Ace Combat 04 (USA).iso", "SLUS-20152");
        g.req.overwrite_local = 1;
        sigil_sync_result *r = NULL;
        if (sigil_restore(&g.req, unit, len, &r) != SIGIL_ERR_UNSUPPORTED_FORMAT || root.writes != 0) {
            fail("folder names", NAMES[i]);
        }
        sigil_sync_result_free(r);
        root_free(&root);
        free(unit);
    }
}

/* Every write and remove sigil makes goes through one gate that refuses a
 * path leaving the save root, whatever built it. */
static void check_writes_stay_inside(void) {
    static const char *const OUT[] = { "../x", "/x", "\\x", "C:/x", "a/../../x", "a\\..\\x", "a/.." };
    static const char *const IN[] = { "a/b..c/x", "..a/x", "a/.b", "a/./x" };
    mem_root root = {0};
    sigil_sync_request req = {0};
    req.save.open = root_open;
    req.save.open_ctx = &root;
    req.write = root_write;
    req.remove = root_remove;
    req.write_ctx = &root;
    for (size_t i = 0; i < sizeof(OUT) / sizeof(OUT[0]); i++) {
        if (sigil_sync_put(&req, OUT[i], (const uint8_t *)"x", 1) != SIGIL_ERR_IO || root.writes != 0 ||
            sigil_sync_drop(&req, OUT[i]) != SIGIL_ERR_IO || root.removes != 0) {
            fail("writes stay inside", OUT[i]);
        }
    }
    for (size_t i = 0; i < sizeof(IN) / sizeof(IN[0]); i++) {
        if (sigil_sync_put(&req, IN[i], (const uint8_t *)"x", 1) != SIGIL_OK || sigil_sync_drop(&req, IN[i]) != SIGIL_OK) {
            fail("writes stay inside", IN[i]);
        }
    }
    root_free(&root);
}

/* Another game's save folder named `name` whose largest file is `size` bytes. */
static bool put_big_foreign_folder(mem_root *root, const char *name, size_t size) {
    sigil_ps2_save save;
    if (!folder_save("champions-of-norrath-aethersx2", &save)) return false;
    memset(save.entry + 0x40, 0, 32);
    memcpy(save.entry + 0x40, name, strlen(name));
    size_t biggest = 0;
    for (size_t i = 1; i < save.file_count; i++) {
        if (sigil_read_le32(save.files[i].entry + 4) > sigil_read_le32(save.files[biggest].entry + 4)) biggest = i;
    }
    uint8_t *grown = (uint8_t *)calloc(1, size);
    bool ok = grown != NULL;
    if (ok) {
        free(save.files[biggest].data);
        save.files[biggest].data = grown;
        save.files[biggest].entry[4] = (uint8_t)size;
        save.files[biggest].entry[5] = (uint8_t)(size >> 8);
        save.files[biggest].entry[6] = (uint8_t)(size >> 16);
        save.files[biggest].entry[7] = (uint8_t)(size >> 24);
        ok = put_save_folder(root, CARD1, &save);
    }
    sigil_ps2_save_free(&save);
    return ok;
}

/* PCSX2 shows a game only its own folders and the system's, so other games'
 * folders don't count against the 8 MB card even when they'd overflow it. */
static void check_folder_other_games_dont_fill_the_card(const sigil_sync_result *first) {
    mem_root root = {0};
    put_superblock(&root, CARD1);
    if (!first || !put_big_foreign_folder(&root, "BASLUS-99998A", 4500000) ||
        !put_big_foreign_folder(&root, "BASLUS-99999B", 4500000)) {
        fail("folder space", "setup failed");
        root_free(&root);
        return;
    }
    size_t before = root.count;
    game g;
    make_standalone(&g, &root, "Ace Combat 04 (USA).iso", "SLUS-20152");
    sigil_sync_result *r = NULL;
    if (sigil_restore(&g.req, first->data, first->len, &r) != SIGIL_OK || root.removes != 0 ||
        files_under(&root, CARD1 "/BASLUS-9999") != before - 1) {
        fail("folder space", "other games' folders filled the card, or changed");
    }
    sigil_sync_result_free(r);
    root_free(&root);
}

/* Slot 1 a file card and slot 2 a folder card: the game's save comes off the
 * folder card and goes back there, and the file card isn't written. */
static void check_folder_beside_file_card(const sigil_sync_result *first) {
    size_t mc01_len = 0, newer_len = 0;
    uint8_t *mc01 = sample("mymc-mc01", &mc01_len);
    uint8_t *newer = first ? newer_unit(first, &newer_len) : NULL;
    mem_root root = {0};
    if (!mc01 || !newer || !put_sample_folder(&root, CARD2, "ace-combat-04-aethersx2")) {
        fail("folder beside file card", "setup failed");
    } else {
        root_put(&root, CARD1, mc01, mc01_len);
        put_superblock(&root, CARD2);
        game g;
        make_standalone(&g, &root, "Ace Combat 04 (USA).iso", "SLUS-20152");
        sigil_sync_result *r = NULL, *placed = NULL;
        if (sigil_collect(&g.req, &r) != SIGIL_OK || strcmp(r->identity_hash, first->identity_hash) != 0) {
            fail("folder beside file card", "the save on the folder card wasn't collected");
        } else {
            g.req.state = r->state;
            g.req.state_len = r->state_len;
            sigil_ps2_card newer_card;
            sigil_ps2_save wanted;
            if (sigil_restore(&g.req, newer, newer_len, &placed) != SIGIL_OK) {
                fail("folder beside file card", "restore failed");
            } else if (load_card(newer, newer_len, &newer_card)) {
                if (extract_named(&newer_card, ACE_COMBAT, &wanted)) {
                    if (!folder_holds(&root, CARD2, &wanted)) fail("folder beside file card", "the newer save isn't in slot 2's folder");
                    sigil_ps2_save_free(&wanted);
                }
                sigil_ps2_card_free(&newer_card);
            }
            mem_file *card1 = root_find(&root, CARD1);
            if (!card1 || card1->len != mc01_len || memcmp(card1->data, mc01, mc01_len) != 0) {
                fail("folder beside file card", "the file card was written");
            }
        }
        sigil_sync_result_free(placed);
        sigil_sync_result_free(r);
    }
    root_free(&root);
    free(mc01);
    free(newer);
}

int main(void) {
    char path[1024];
    if (corpus_platform_path("ps2", "manifest.tsv", path, sizeof(path)) != 0 || corpus_load(path, &g_manifest) != 0) {
        fprintf(stderr, "SKIP: no ps2 manifest\n");
        return TEST_SKIP;
    }
    if (!corpus_present(&g_manifest, "ps2", "mymc-mc01")) {
        fprintf(stderr, "SKIP: ps2 samples missing\n");
        corpus_free(&g_manifest);
        return TEST_SKIP;
    }

    sigil_sync_result *first = NULL;
    check_collect_shared(&first);
    check_collect_deterministic(first);
    check_times_are_not_a_change(first);
    check_restore_keeps_other_games(first);
    check_restore_into_empty_root(first);
    check_unreadable_card(first);
    check_broken_own_save(first);
    check_restore_foreign_unit();
    check_folder_collect(first);
    check_folder_restore(first);
    check_folder_damage(first);
    check_unpackable_folder(first);
    check_superblock_rules(first);
    check_memcard_filters(first);
    check_equal_restore(first);
    check_folder_writes(first);
    check_refusal_spans_slots();
    check_folder_companion(first);
    check_system_folder_counts(first);
    check_faulty_writes(first);
    check_folder_keeps_modes();
    check_folder_removes(first);
    check_folder_names_stay_inside();
    check_writes_stay_inside();
    check_folder_beside_file_card(first);
    check_folder_other_games_dont_fill_the_card(first);
    sigil_sync_result_free(first);

    corpus_free(&g_manifest);
    printf("ps2 sync: %d failures\n", g_fails);
    return corpus_exit(g_fails);
}
