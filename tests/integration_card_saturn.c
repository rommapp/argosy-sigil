// SPDX-License-Identifier: MPL-2.0
#include "save_corpus.h"
#include "card_saturn.h"

#define TEST_SKIP 77
#define PLATFORM "saturn"

static int g_fails = 0;
static int g_volumes = 0;

static void fail(const char *id, const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", id, what);
    g_fails++;
}

typedef struct { const uint8_t *data; size_t len; } mem_ctx;

static int mem_read(void *ctx, uint64_t off, void *buf, size_t len) {
    mem_ctx *m = (mem_ctx *)ctx;
    if (off >= m->len) return 0;
    size_t n = m->len - (size_t)off < len ? m->len - (size_t)off : len;
    memcpy(buf, m->data + off, n);
    return (int)n;
}

static int64_t mem_size(void *ctx) { return (int64_t)((mem_ctx *)ctx)->len; }

static void md5_hex(const uint8_t *data, size_t len, char out[33]) {
    sigil_md5 m;
    uint8_t digest[16];
    sigil_md5_init(&m);
    sigil_md5_update(&m, data, len);
    sigil_md5_final(&m, digest);
    sigil_md5_hex(digest, out);
}

static void entry_md5(const sigil_saturn_volume *vol, const sigil_card_entry *e, char out[33]) {
    out[0] = '\0';
    uint8_t *data = NULL;
    size_t len = 0;
    if (sigil_saturn_entry_data(vol, e->first_block, &data, &len) == SIGIL_OK) md5_hex(data, len, out);
    free(data);
}

static const sigil_card_entry *find_entry(const sigil_card_listing *l, const char *name) {
    for (size_t i = 0; i < l->entry_count; i++) {
        if (strcmp(l->entries[i].name, name) == 0) return &l->entries[i];
    }
    return NULL;
}

static void check_expected_entries(const char *where, const char *id, const char *path,
                                   const corpus_table *entries, const sigil_card_listing *listing,
                                   const sigil_saturn_volume *vol) {
    size_t expected = 0;
    for (size_t r = 0; r < entries->nrows; r++) {
        if (strcmp(corpus_get(entries, r, "id"), id) != 0) continue;
        if (strcmp(corpus_get(entries, r, "path"), path) != 0) continue;
        expected++;

        const char *name = corpus_get(entries, r, "entry");
        const sigil_card_entry *e = find_entry(listing, name);
        char label[256];
        snprintf(label, sizeof(label), "%s %s", where, name);
        if (!e) { fail(label, "entry not listed"); continue; }
        if (e->owner_id[0] != '\0') fail(label, "owner_id is not empty");
        if (strtoul(corpus_get(entries, r, "blocks"), NULL, 10) != e->blocks) fail(label, "block count differs");
        char md5[33];
        entry_md5(vol, e, md5);
        if (strcmp(md5, corpus_get(entries, r, "data_md5")) != 0) fail(label, "data md5 differs");
    }
    if (expected != listing->entry_count) fail(where, "listing holds entries entries.tsv doesn't");
}

typedef struct {
    const char *id;
    const char *path;
    uint32_t    free_blocks;
} volume_fact;

/* Internal volumes hold 510 usable blocks (512 less the signature block and
 * the unused block 1), carts 1022, the 4 MiB Yaba Sanshiro volume 65534.
 * Free counts are those less the blocks entries.tsv records, and for the Yaba
 * Sanshiro volume less the 877 used blocks its manifest row states. */
static const volume_fact VOLUME_FACTS[] = {
    { "empty-bkr",           "Empty save.bkr",                                          510 },
    { "hyper-duel-bkr",      "Hyper Duel (Japan).bkr",                                  505 },
    { "sf3-scn3-bkr",        "Shining Force III Scenario 3 (English v25.1).bkr",        89 },
    { "sf3-scn3-bcr",        "Shining Force III Scenario 3 (English v25.1).bcr",        973 },
    { "daytona-cce-bcr",     "Daytona USA - Championship Circuit Edition (USA).bcr",    890 },
    { "yabasanshiro-backup", "backup.bin",                                              64657 },
};

static void check_facts(const char *id, const char *path, const sigil_card_listing *listing) {
    if (listing->corrupt_count != 0) fail(id, "a sample entry counted corrupt");
    if (listing->free_slots != listing->free_blocks) fail(id, "free slots differ from free blocks");
    for (size_t i = 0; i < sizeof(VOLUME_FACTS) / sizeof(VOLUME_FACTS[0]); i++) {
        if (strcmp(VOLUME_FACTS[i].id, id) != 0 || strcmp(VOLUME_FACTS[i].path, path) != 0) continue;
        if (listing->free_blocks != VOLUME_FACTS[i].free_blocks) fail(id, "free block count differs");
    }
}

typedef struct {
    uint8_t **bup;
    size_t   *len;
    size_t    count;
} save_set;

static void save_set_free(save_set *s) {
    for (size_t i = 0; i < s->count; i++) free(s->bup[i]);
    free(s->bup);
    free(s->len);
    memset(s, 0, sizeof(*s));
}

static bool extract_all(const sigil_saturn_volume *vol, const sigil_card_listing *l, save_set *out) {
    memset(out, 0, sizeof(*out));
    out->bup = (uint8_t **)calloc(l->entry_count + 1, sizeof(uint8_t *));
    out->len = (size_t *)calloc(l->entry_count + 1, sizeof(size_t));
    if (!out->bup || !out->len) { save_set_free(out); return false; }
    for (size_t i = 0; i < l->entry_count; i++) {
        if (sigil_saturn_extract(vol, l->entries[i].first_block, &out->bup[i], &out->len[i]) != SIGIL_OK) {
            save_set_free(out);
            return false;
        }
        out->count++;
    }
    return true;
}

static bool build_volume(const sigil_saturn_volume *like, const save_set *saves, sigil_saturn_volume *out) {
    if (sigil_saturn_volume_format(out, like->size, &like->storage) != SIGIL_OK) return false;
    for (size_t i = 0; i < saves->count; i++) {
        if (sigil_saturn_inject(out, saves->bup[i], saves->len[i]) != SIGIL_OK) return false;
    }
    return true;
}

static bool written_equal(const sigil_saturn_volume *a, const sigil_saturn_volume *b) {
    uint8_t *wa = NULL, *wb = NULL;
    size_t la = 0, lb = 0;
    bool same = sigil_saturn_volume_write(a, &wa, &la) == SIGIL_OK &&
                sigil_saturn_volume_write(b, &wb, &lb) == SIGIL_OK &&
                la == lb && memcmp(wa, wb, la) == 0;
    free(wa);
    free(wb);
    return same;
}

/* Rebuilding a volume from its own saves keeps every save, builds the same
 * volume twice, and leaves each save verifiable. */
static void check_rebuild(const char *id, const char *path, const corpus_table *entries,
                          const sigil_saturn_volume *vol, const sigil_card_listing *listing) {
    save_set saves;
    if (!extract_all(vol, listing, &saves)) { fail(id, "extract failed"); return; }
    sigil_saturn_volume built = {0}, again = {0};
    sigil_card_listing *rebuilt = NULL;
    char label[256];
    snprintf(label, sizeof(label), "%s (rebuilt)", id);
    if (!build_volume(vol, &saves, &built) || !build_volume(vol, &saves, &again)) {
        fail(label, "rebuild failed");
    } else if (!written_equal(&built, &again)) {
        fail(label, "two builds from the same saves differ");
    } else if (sigil_saturn_list(&built, &rebuilt) != SIGIL_OK) {
        fail(label, "rebuilt volume did not list");
    } else {
        check_expected_entries(label, id, path, entries, rebuilt, &built);
        for (size_t i = 0; i < saves.count; i++) {
            if (sigil_saturn_verify(&built, saves.bup[i], saves.len[i]) != SIGIL_OK) fail(label, "a save did not verify");
        }
    }
    sigil_card_listing_free(rebuilt);
    sigil_saturn_volume_free(&built);
    sigil_saturn_volume_free(&again);
    save_set_free(&saves);
}

static uint8_t *sample_bytes(const char *id, const char *path, size_t *len) {
    char full[1024];
    if (corpus_sample_path(PLATFORM, id, path, full, sizeof(full)) != 0) return NULL;
    return corpus_read_file(full, len);
}

static bool load_volume(const char *id, const char *path, sigil_saturn_volume *vol) {
    char full[1024];
    if (corpus_sample_path(PLATFORM, id, path, full, sizeof(full)) != 0) return false;
    sigil_io *io = sigil_io_open_file(full);
    if (!io) return false;
    int rc = sigil_saturn_volume_load(io, vol);
    sigil_io_close(io);
    return rc == SIGIL_OK;
}

typedef struct {
    const char *id;
    const char *gzip_path;
    const char *plain_path;
} gzip_pair;

/* Standalone Mednafen's gzip header, read from both .bcr samples:
 * 1f 8b 08 00 00 00 00 00 00 13. */
static const uint8_t MEDNAFEN_GZIP_HEADER[10] = { 0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x13 };

static const gzip_pair GZIP_PAIRS[] = {
    { "sf3-scn3-bcr", "Shining Force III Scenario 3 (English v25.1).bcr",
      "Shining Force III Scenario 3 (English v25.1)-uncompressed.bcr" },
    { "daytona-cce-bcr", "Daytona USA - Championship Circuit Edition (USA).bcr",
      "Daytona USA - Championship Circuit Edition (USA)-uncompressed.bcr" },
};

/* An unchanged volume goes back in its own form: raw and expanded volumes
 * byte for byte, gzip volumes as a gzip member with Mednafen's header whose
 * contents are the volume. */
static void check_write_back(const char *id, const char *path, const sigil_saturn_volume *vol) {
    size_t orig_len = 0;
    uint8_t *orig = sample_bytes(id, path, &orig_len);
    uint8_t *out = NULL;
    size_t out_len = 0;
    if (!orig || sigil_saturn_volume_write(vol, &out, &out_len) != SIGIL_OK) {
        fail(id, "write failed");
    } else if (!vol->storage.gzip) {
        if (out_len != orig_len || memcmp(out, orig, orig_len) != 0) fail(id, "written volume differs from the file");
    } else {
        if (out_len < sizeof(MEDNAFEN_GZIP_HEADER) || memcmp(out, MEDNAFEN_GZIP_HEADER, sizeof(MEDNAFEN_GZIP_HEADER)) != 0) {
            fail(id, "written gzip lost Mednafen's header");
        }
        mem_ctx m = { out, out_len };
        sigil_io io = { mem_read, mem_size, NULL, &m };
        sigil_saturn_volume back = {0};
        if (sigil_saturn_volume_load(&io, &back) != SIGIL_OK || !back.storage.gzip ||
            back.size != vol->size || memcmp(back.data, vol->data, vol->size) != 0) {
            fail(id, "written gzip does not read back as the same volume");
        }
        sigil_saturn_volume_free(&back);
    }
    free(out);
    free(orig);
}

static void check_gzip_pairs(void) {
    for (size_t i = 0; i < sizeof(GZIP_PAIRS) / sizeof(GZIP_PAIRS[0]); i++) {
        const gzip_pair *p = &GZIP_PAIRS[i];
        size_t plain_len = 0;
        uint8_t *plain = sample_bytes(p->id, p->plain_path, &plain_len);
        sigil_saturn_volume vol = {0};
        if (!plain || !load_volume(p->id, p->gzip_path, &vol)) { free(plain); continue; }
        if (!vol.storage.gzip || vol.storage.expanded) fail(p->id, "gzip volume not read as gzip");
        if (vol.size != plain_len || memcmp(vol.data, plain, plain_len) != 0) {
            fail(p->id, "gunzipped volume differs from the uncompressed sample");
        }
        sigil_saturn_volume_free(&vol);
        free(plain);
    }
}

static void check_volume(const char *id, const char *path, const corpus_table *entries) {
    char full[1024];
    if (corpus_sample_path(PLATFORM, id, path, full, sizeof(full)) != 0) return;
    sigil_io *io = sigil_io_open_file(full);
    if (!io) return;
    g_volumes++;

    sigil_saturn_volume vol = {0};
    sigil_card_listing *listing = NULL;
    if (sigil_saturn_volume_load(io, &vol) != SIGIL_OK || sigil_saturn_list(&vol, &listing) != SIGIL_OK) {
        fail(id, "volume did not load");
    } else {
        check_expected_entries(id, id, path, entries, listing, &vol);
        check_facts(id, path, listing);
        check_rebuild(id, path, entries, &vol, listing);
        check_write_back(id, path, &vol);
        sigil_card_listing *public_listing = NULL;
        if (sigil_card_list(io, &public_listing) != SIGIL_OK ||
            public_listing->format != SIGIL_CARD_FORMAT_SATURN_BACKUP ||
            public_listing->entry_count != listing->entry_count) {
            fail(id, "sigil_card_list disagrees with the internal listing");
        }
        sigil_card_listing_free(public_listing);
    }
    sigil_card_listing_free(listing);
    sigil_saturn_volume_free(&vol);
    sigil_io_close(io);
}

static void check_not_volume(const char *id, const char *path) {
    char full[1024];
    if (corpus_sample_path(PLATFORM, id, path, full, sizeof(full)) != 0) return;
    sigil_io *io = sigil_io_open_file(full);
    if (!io) return;
    sigil_saturn_volume vol = {0};
    if (sigil_saturn_volume_load(io, &vol) != SIGIL_ERR_UNSUPPORTED_FORMAT) fail(id, "a .BUP read as a volume");
    sigil_saturn_volume_free(&vol);
    sigil_io_close(io);
}

/* .BUP header bytes a volume doesn't keep: the save id and statistics
 * (0x04-0x0F), the name's twelfth byte (0x1B), and the block count, padding,
 * second date and reserved bytes (0x30-0x3F). */
static bool bup_field_kept(size_t at) {
    if (at >= 0x04 && at <= 0x0F) return false;
    if (at == 0x1B) return false;
    if (at >= 0x30 && at <= 0x3F) return false;
    return true;
}

typedef struct {
    const char *id;
    const char *path;
    size_t      volume_size;
    bool        blocks_as_recorded;
    bool        byte_exact;
} bup_case;

/* TGKRPLY_RP1 takes 685 64-byte blocks, more than a 32 KiB volume holds, so
 * it goes on the 4 MiB form. TOKIMEKI_01 records 0 blocks and SFORCE31 52
 * (0x0034 at 0x30), a count for a device with other block sizes, so the
 * listing's count can't match theirs. THREE_DIRTY and TGKRPLY_RP1 carry zero
 * id and statistics, a NUL name terminator and a second date equal to the
 * first (hex at 0x04-0x0F, 0x1B, 0x28 and 0x34), so they come back byte for byte. */
static const bup_case BUP_CASES[] = {
    { "draculax-bup",            "DRACULAX.BUP",       SATURN_INTERNAL_SIZE,     true,  false },
    { "sforce31-bup",            "SFORCE31.BUP",       SATURN_INTERNAL_SIZE,     false, false },
    { "pandra-zwei-bup",         "PANDRA_ZWEI.BUP",    SATURN_INTERNAL_SIZE,     true,  false },
    { "pandra-zwei-bup",         "PANDRA_ZWEI_01.BUP", SATURN_INTERNAL_SIZE,     true,  false },
    { "pandra-zwei-bup",         "PANDRA_ZWEI_02.BUP", SATURN_INTERNAL_SIZE,     true,  false },
    { "pandra-zwei-bup",         "PANDRA_ZWEI_03.BUP", SATURN_INTERNAL_SIZE,     true,  false },
    { "touge-king-bup",          "TGKRPLY_RP1.BUP",    SATURN_YABASANSHIRO_SIZE, true,  true },
    { "three-dirty-dwarves-bup", "THREE_DIRTY.BUP",    SATURN_INTERNAL_SIZE,     true,  true },
    { "tokimeki-bup",            "TOKIMEKI_01.BUP",    SATURN_INTERNAL_SIZE,     false, false },
    { "tokimeki-bup",            "TOKIMEKI_99.BUP",    SATURN_INTERNAL_SIZE,     true,  false },
};

static const char *entries_value(const corpus_table *entries, const char *id, const char *path, const char *col) {
    for (size_t r = 0; r < entries->nrows; r++) {
        if (strcmp(corpus_get(entries, r, "id"), id) == 0 && strcmp(corpus_get(entries, r, "path"), path) == 0) {
            return corpus_get(entries, r, col);
        }
    }
    return NULL;
}

/* A .BUP written by another tool goes onto a volume and comes back with
 * every field a volume keeps. */
static void check_bup_round_trip(const corpus_table *entries) {
    static const sigil_bram_storage raw = {0};
    for (size_t i = 0; i < sizeof(BUP_CASES) / sizeof(BUP_CASES[0]); i++) {
        const bup_case *c = &BUP_CASES[i];
        size_t len = 0;
        uint8_t *bup = sample_bytes(c->id, c->path, &len);
        if (!bup) continue;
        const char *name = entries_value(entries, c->id, c->path, "entry");
        const char *blocks = entries_value(entries, c->id, c->path, "blocks");
        const char *md5 = entries_value(entries, c->id, c->path, "data_md5");
        sigil_saturn_volume vol = {0};
        sigil_card_listing *l = NULL;
        uint8_t *back = NULL;
        size_t back_len = 0;
        if (!name || !blocks || !md5 || sigil_saturn_volume_format(&vol, c->volume_size, &raw) != SIGIL_OK ||
            sigil_saturn_inject(&vol, bup, len) != SIGIL_OK || sigil_saturn_list(&vol, &l) != SIGIL_OK ||
            l->entry_count != 1 ||
            sigil_saturn_extract(&vol, l->entries[0].first_block, &back, &back_len) != SIGIL_OK) {
            fail(c->path, "inject or extract failed");
        } else {
            char data_md5[33];
            entry_md5(&vol, &l->entries[0], data_md5);
            if (strcmp(l->entries[0].name, name) != 0) fail(c->path, "name differs");
            if (strcmp(data_md5, md5) != 0) fail(c->path, "data md5 differs");
            if (c->blocks_as_recorded && l->entries[0].blocks != strtoul(blocks, NULL, 10)) fail(c->path, "block count differs");
            if (sigil_saturn_verify(&vol, bup, len) != SIGIL_OK) fail(c->path, "the .BUP did not verify");
            if (back_len != len) {
                fail(c->path, "extracted .BUP length differs");
            } else {
                for (size_t at = 0; at < len; at++) {
                    if (bup_field_kept(at) && back[at] != bup[at]) { fail(c->path, "a kept field changed"); break; }
                }
                if (c->byte_exact && memcmp(back, bup, len) != 0) fail(c->path, "the .BUP changed on its way through a volume");
            }
        }
        free(back);
        sigil_card_listing_free(l);
        sigil_saturn_volume_free(&vol);
        free(bup);
    }
}

/* The empty-bkr sample is a blank internal volume; the cart and Yaba Sanshiro
 * samples show the first two blocks of theirs. */
static void check_format(void) {
    static const sigil_bram_storage raw = {0};
    size_t len = 0;
    uint8_t *blank = sample_bytes("empty-bkr", "Empty save.bkr", &len);
    sigil_saturn_volume vol = {0};
    if (blank) {
        if (sigil_saturn_volume_format(&vol, SATURN_INTERNAL_SIZE, &raw) != SIGIL_OK ||
            len != vol.size || memcmp(vol.data, blank, len) != 0) {
            fail("empty-bkr", "a formatted volume differs from the blank sample");
        }
        sigil_saturn_volume_free(&vol);
        free(blank);
    }

    uint8_t *cart = sample_bytes("sf3-scn3-bcr", "Shining Force III Scenario 3 (English v25.1)-uncompressed.bcr", &len);
    if (cart) {
        if (sigil_saturn_volume_format(&vol, SATURN_CART_SIZE, &raw) != SIGIL_OK ||
            len != vol.size || memcmp(vol.data, cart, 2 * 512) != 0) {
            fail("sf3-scn3-bcr", "a formatted cart's first blocks differ from the sample");
        }
        sigil_saturn_volume_free(&vol);
        free(cart);
    }

    uint8_t *yaba = sample_bytes("yabasanshiro-backup", "backup.bin", &len);
    if (yaba) {
        sigil_bram_storage expanded = { false, {0}, true, 0xFF };
        uint8_t *out = NULL;
        size_t out_len = 0;
        if (sigil_saturn_volume_format(&vol, SATURN_YABASANSHIRO_SIZE, &expanded) != SIGIL_OK ||
            sigil_saturn_volume_write(&vol, &out, &out_len) != SIGIL_OK ||
            out_len != len || memcmp(out, yaba, 2 * 2 * 64) != 0) {
            fail("yabasanshiro-backup", "a formatted expanded volume's first blocks differ from the sample");
        }
        free(out);
        sigil_saturn_volume_free(&vol);
        free(yaba);
    }
}

static void expect_refused(const char *label, sigil_saturn_volume *vol, const uint8_t *bup, size_t len, int want) {
    uint8_t *before = (uint8_t *)malloc(vol->size);
    if (!before) return;
    memcpy(before, vol->data, vol->size);
    if (sigil_saturn_inject(vol, bup, len) != want) fail(label, "inject was not refused");
    if (memcmp(before, vol->data, vol->size) != 0) fail(label, "a refused inject changed the volume");
    free(before);
}

/* SFORCE31 needs 223 internal blocks; sf3-scn3-bkr has 89 free. TGKRPLY_RP1
 * needs 685; an empty internal volume has 510. */
static void check_refusals(void) {
    static const sigil_bram_storage raw = {0};
    size_t sf31_len = 0, tgk_len = 0, pz_len = 0, pz1_len = 0;
    uint8_t *sf31 = sample_bytes("sforce31-bup", "SFORCE31.BUP", &sf31_len);
    uint8_t *tgk = sample_bytes("touge-king-bup", "TGKRPLY_RP1.BUP", &tgk_len);
    uint8_t *pz = sample_bytes("pandra-zwei-bup", "PANDRA_ZWEI.BUP", &pz_len);
    uint8_t *pz1 = sample_bytes("pandra-zwei-bup", "PANDRA_ZWEI_01.BUP", &pz1_len);
    sigil_saturn_volume vol = {0};

    if (sf31 && load_volume("sf3-scn3-bkr", "Shining Force III Scenario 3 (English v25.1).bkr", &vol)) {
        expect_refused("sf3-scn3-bkr", &vol, sf31, sf31_len, SIGIL_ERR_NOT_FOUND);
        sigil_saturn_volume_free(&vol);
    }
    if (tgk && sigil_saturn_volume_format(&vol, SATURN_INTERNAL_SIZE, &raw) == SIGIL_OK) {
        expect_refused("TGKRPLY_RP1.BUP", &vol, tgk, tgk_len, SIGIL_ERR_NOT_FOUND);
        sigil_saturn_volume_free(&vol);
    }
    if (pz && pz1 && sigil_saturn_volume_format(&vol, SATURN_INTERNAL_SIZE, &raw) == SIGIL_OK) {
        if (sigil_saturn_inject(&vol, pz, pz_len) != SIGIL_OK) fail("PANDRA_ZWEI.BUP", "inject failed");
        expect_refused("PANDRA_ZWEI_01.BUP", &vol, pz1, pz1_len, SIGIL_ERR_INVALID_ARG);
        sigil_saturn_volume_free(&vol);
    }
    if (pz) {
        uint8_t *cut = (uint8_t *)malloc(pz_len);
        if (cut && sigil_saturn_volume_format(&vol, SATURN_INTERNAL_SIZE, &raw) == SIGIL_OK) {
            memcpy(cut, pz, pz_len);
            expect_refused("PANDRA_ZWEI.BUP truncated", &vol, cut, pz_len - 1, SIGIL_ERR_UNSUPPORTED_FORMAT);
            cut[0] = 'X';
            expect_refused("PANDRA_ZWEI.BUP bad magic", &vol, cut, pz_len, SIGIL_ERR_UNSUPPORTED_FORMAT);
            sigil_saturn_volume_free(&vol);
        }
        free(cut);
    }
    free(sf31);
    free(tgk);
    free(pz);
    free(pz1);
}

/* Injecting into the shared Yaba Sanshiro volume leaves its 25 saves as they
 * were; deleting RAYEARTH_00 (75 blocks per entries.tsv) frees its blocks and
 * leaves the rest, the injected save included, verifiable. */
static void check_inject_and_delete(void) {
    size_t bup_len = 0;
    uint8_t *bup = sample_bytes("draculax-bup", "DRACULAX.BUP", &bup_len);
    sigil_saturn_volume vol = {0};
    if (!bup || !load_volume("yabasanshiro-backup", "backup.bin", &vol)) { free(bup); return; }
    const char *id = "yabasanshiro-backup";
    sigil_card_listing *before = NULL, *after = NULL, *deleted = NULL;
    save_set kept = {0};
    if (sigil_saturn_list(&vol, &before) != SIGIL_OK || !extract_all(&vol, before, &kept) ||
        sigil_saturn_inject(&vol, bup, bup_len) != SIGIL_OK || sigil_saturn_list(&vol, &after) != SIGIL_OK) {
        fail(id, "inject failed");
    } else {
        if (after->entry_count != before->entry_count + 1) fail(id, "entry count after inject");
        if (after->free_blocks + 77 != before->free_blocks) fail(id, "free blocks after inject");
        for (size_t i = 0; i < kept.count; i++) {
            if (sigil_saturn_verify(&vol, kept.bup[i], kept.len[i]) != SIGIL_OK) fail(id, "an existing save changed");
        }
        if (sigil_saturn_verify(&vol, bup, bup_len) != SIGIL_OK) fail(id, "injected save did not verify");

        const sigil_card_entry *ray = find_entry(after, "RAYEARTH_00");
        if (!ray || sigil_saturn_delete(&vol, ray->first_block) != SIGIL_OK ||
            sigil_saturn_list(&vol, &deleted) != SIGIL_OK) {
            fail(id, "delete failed");
        } else {
            if (find_entry(deleted, "RAYEARTH_00")) fail(id, "deleted save still listed");
            if (deleted->free_blocks != after->free_blocks + 75) fail(id, "delete did not free the save's blocks");
            if (sigil_saturn_verify(&vol, bup, bup_len) != SIGIL_OK) fail(id, "delete touched the injected save");
            size_t verified = 0;
            for (size_t i = 0; i < kept.count; i++) {
                if (sigil_saturn_verify(&vol, kept.bup[i], kept.len[i]) == SIGIL_OK) verified++;
            }
            if (verified + 1 != kept.count) fail(id, "delete touched another save");
        }
    }
    sigil_card_listing_free(before);
    sigil_card_listing_free(after);
    sigil_card_listing_free(deleted);
    save_set_free(&kept);
    sigil_saturn_volume_free(&vol);
    free(bup);
}

/* HYPERDUEL_0's block list starts at 0xA2 in the sample: 0003 0004 0005 0006
 * 0000. Pointing its first link at block 1, or at the archive block itself,
 * or dropping its terminator into the data, breaks the entry. */
static void check_corrupt_list(void) {
    static const struct { size_t at; uint8_t value; const char *what; } BREAKS[] = {
        { 0xA3, 0x01, "a link to reserved block 1" },
        { 0xA3, 0x02, "a link back to the archive block" },
        { 0xA5, 0x03, "a repeated link" },
        { 0xAB, 0x07, "a list running into the data" },
    };
    for (size_t i = 0; i < sizeof(BREAKS) / sizeof(BREAKS[0]); i++) {
        sigil_saturn_volume vol = {0};
        if (!load_volume("hyper-duel-bkr", "Hyper Duel (Japan).bkr", &vol)) return;
        vol.data[BREAKS[i].at] = BREAKS[i].value;
        sigil_card_listing *l = NULL;
        if (sigil_saturn_list(&vol, &l) != SIGIL_OK || l->entry_count != 0 || l->corrupt_count != 1) {
            fail("hyper-duel-bkr", BREAKS[i].what);
        }
        sigil_card_listing_free(l);
        sigil_saturn_volume_free(&vol);
    }
}

int main(void) {
    char path[1024];
    corpus_table manifest, entries;
    if (corpus_platform_path(PLATFORM, "manifest.tsv", path, sizeof(path)) != 0 ||
        corpus_load(path, &manifest) != 0) {
        fprintf(stderr, "SKIP: no saturn manifest\n");
        return TEST_SKIP;
    }
    if (corpus_platform_path(PLATFORM, "entries.tsv", path, sizeof(path)) != 0 ||
        corpus_load(path, &entries) != 0) {
        corpus_free(&manifest);
        fprintf(stderr, "SKIP: no saturn entries\n");
        return TEST_SKIP;
    }

    for (size_t r = 0; r < manifest.nrows; r++) {
        const char *kind = corpus_get(&manifest, r, "kind");
        const char *id = corpus_get(&manifest, r, "id");
        const char *file = corpus_get(&manifest, r, "path");
        if (!kind || !id || !file) continue;
        if (strcmp(kind, "volume") == 0) check_volume(id, file, &entries);
        else if (strcmp(kind, "wrapped") == 0) check_not_volume(id, file);
    }
    check_gzip_pairs();
    check_bup_round_trip(&entries);
    check_format();
    check_refusals();
    check_inject_and_delete();
    check_corrupt_list();
    corpus_free(&manifest);
    corpus_free(&entries);

    printf("saturn volumes: %d checked, %d failures\n", g_volumes, g_fails);
    if (g_fails) return 1;
    return g_volumes ? 0 : TEST_SKIP;
}
