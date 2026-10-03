// SPDX-License-Identifier: MPL-2.0
#include "save_corpus.h"
#include "card_segacd.h"

#define TEST_SKIP 77
#define PLATFORM "segacd"

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

static void entry_md5(const sigil_segacd_volume *vol, const sigil_card_entry *e, char out[33]) {
    out[0] = '\0';
    uint8_t *data = NULL;
    size_t len = 0;
    if (sigil_segacd_entry_data(vol, e->first_block, &data, &len) == SIGIL_OK) {
        sigil_md5 m;
        uint8_t digest[16];
        sigil_md5_init(&m);
        sigil_md5_update(&m, data, len);
        sigil_md5_final(&m, digest);
        sigil_md5_hex(digest, out);
    }
    free(data);
}

static const sigil_card_entry *find_entry(const sigil_card_listing *l, const char *name) {
    for (size_t i = 0; i < l->entry_count; i++) {
        if (strcmp(l->entries[i].name, name) == 0) return &l->entries[i];
    }
    return NULL;
}

/* One sample file can hold two volumes (picodrive's internal and cart parts);
 * its entries.tsv rows are checked against both listings together. */
typedef struct {
    const sigil_segacd_volume *vol[2];
    const sigil_card_listing  *listing[2];
    size_t                     count;
} volume_pair;

static void check_expected_entries(const char *where, const char *id, const char *path,
                                   const corpus_table *entries, const volume_pair *p) {
    size_t expected = 0, listed = 0;
    for (size_t v = 0; v < p->count; v++) listed += p->listing[v]->entry_count;
    for (size_t r = 0; r < entries->nrows; r++) {
        if (strcmp(corpus_get(entries, r, "id"), id) != 0) continue;
        if (strcmp(corpus_get(entries, r, "path"), path) != 0) continue;
        expected++;

        const char *name = corpus_get(entries, r, "entry");
        char label[256];
        snprintf(label, sizeof(label), "%s %s", where, name);
        const sigil_card_entry *e = NULL;
        const sigil_segacd_volume *vol = NULL;
        for (size_t v = 0; v < p->count && !e; v++) {
            e = find_entry(p->listing[v], name);
            vol = p->vol[v];
        }
        if (!e) { fail(label, "entry not listed"); continue; }
        if (e->owner_id[0] != '\0') fail(label, "owner_id is not empty");
        if (strtoul(corpus_get(entries, r, "blocks"), NULL, 10) != e->blocks) fail(label, "block count differs");
        char md5[33];
        entry_md5(vol, e, md5);
        if (strcmp(md5, corpus_get(entries, r, "data_md5")) != 0) fail(label, "data md5 differs");
    }
    if (expected != listed) fail(where, "listing holds entries entries.tsv doesn't");
}

typedef struct {
    const char *id;
    bool        cart_part;
    uint32_t    free_blocks;
    uint32_t    total_blocks;
} volume_fact;

/* Free counts read from each format block's first count copy (volume end
 * less 0x30): 0x007D, 0x1FFD, 0x0018, 0x002C, 0x0055, 0x007C and, for the
 * cart part of the picodrive file at 0x11FD0, 0x026F. Popful Mail's volume
 * stores 0x1FFD, the count of an empty cart, beside its one 13-block save;
 * the BIOS count for that save is 8192 blocks less block 0, the save, one
 * directory block, the one held for the next entry and the format block. */
static const volume_fact VOLUME_FACTS[] = {
    { "blank-internal",          false, 125,  125 },
    { "blank-cart-brm",          false, 8189, 8189 },
    { "multi-titles-brm",        false, 24,   125 },
    { "dark-wizard-brm",         false, 44,   125 },
    { "lunar-ecc-brm",           false, 85,   125 },
    { "sfcd-internal-plus-cart", false, 124,  125 },
    { "sfcd-internal-plus-cart", true,  623,  1021 },
    { "popful-mail-cart-brm",    false, 8176, 8189 },
};

static void check_facts(const char *id, bool cart_part, const sigil_card_listing *listing) {
    if (listing->corrupt_count != 0) fail(id, "a sample entry counted corrupt");
    for (size_t i = 0; i < sizeof(VOLUME_FACTS) / sizeof(VOLUME_FACTS[0]); i++) {
        if (strcmp(VOLUME_FACTS[i].id, id) != 0 || VOLUME_FACTS[i].cart_part != cart_part) continue;
        if (listing->free_blocks != VOLUME_FACTS[i].free_blocks) fail(id, "free block count differs");
        if (listing->total_blocks != VOLUME_FACTS[i].total_blocks) fail(id, "total block count differs");
    }
}

typedef struct {
    uint8_t **unit;
    size_t   *len;
    size_t    count;
} save_set;

static void save_set_free(save_set *s) {
    for (size_t i = 0; i < s->count; i++) free(s->unit[i]);
    free(s->unit);
    free(s->len);
    memset(s, 0, sizeof(*s));
}

static bool extract_all(const sigil_segacd_volume *vol, const sigil_card_listing *l, save_set *out) {
    memset(out, 0, sizeof(*out));
    out->unit = (uint8_t **)calloc(l->entry_count + 1, sizeof(uint8_t *));
    out->len = (size_t *)calloc(l->entry_count + 1, sizeof(size_t));
    if (!out->unit || !out->len) { save_set_free(out); return false; }
    for (size_t i = 0; i < l->entry_count; i++) {
        size_t len = sigil_segacd_unit_size(l->entries[i].blocks);
        uint8_t *unit = (uint8_t *)malloc(len);
        if (!unit || sigil_segacd_extract(vol, l->entries[i].first_block, l->entries[i].blocks, unit) != SIGIL_OK) {
            free(unit);
            save_set_free(out);
            return false;
        }
        out->unit[out->count] = unit;
        out->len[out->count++] = len;
    }
    return true;
}

static bool build_volume(const sigil_segacd_volume *like, const save_set *saves, sigil_segacd_volume *out) {
    if (sigil_segacd_volume_format(out, like->size, &like->storage) != SIGIL_OK) return false;
    for (size_t i = 0; i < saves->count; i++) {
        if (sigil_segacd_inject(out, saves->unit[i], saves->len[i]) != SIGIL_OK) return false;
    }
    return true;
}

/* The BIOS packs saves from block 1 and pairs directory entries per block, so
 * a rebuilt volume matches the original in every data block the saves use and
 * every directory block holding two entries. The one exception is the last
 * directory block of an odd count, whose second half keeps whatever it held. */
static void check_same_layout(const char *label, const sigil_segacd_volume *orig,
                              const sigil_segacd_volume *built, const sigil_card_listing *listing) {
    size_t used = 0;
    for (size_t i = 0; i < listing->entry_count; i++) used += listing->entries[i].blocks;
    size_t data_bytes = used * SEGACD_BLOCK_SIZE;
    if (memcmp(orig->data + SEGACD_BLOCK_SIZE, built->data + SEGACD_BLOCK_SIZE, data_bytes) != 0) {
        fail(label, "rebuilt data blocks differ from the original");
    }
    size_t full = listing->entry_count / 2;
    size_t dir_bytes = full * SEGACD_BLOCK_SIZE;
    size_t dir_start = orig->size - SEGACD_BLOCK_SIZE - dir_bytes;
    if (memcmp(orig->data + dir_start, built->data + dir_start, dir_bytes) != 0) {
        fail(label, "rebuilt directory blocks differ from the original");
    }
}

static bool written_equal(const sigil_segacd_volume *a, const sigil_segacd_volume *b) {
    uint8_t *wa = NULL, *wb = NULL;
    size_t la = 0, lb = 0;
    bool same = sigil_segacd_volume_write(a, &wa, &la) == SIGIL_OK &&
                sigil_segacd_volume_write(b, &wb, &lb) == SIGIL_OK &&
                la == lb && memcmp(wa, wb, la) == 0;
    free(wa);
    free(wb);
    return same;
}

static void check_rebuild(const char *id, const char *path, const corpus_table *entries,
                          const sigil_segacd_volume *vol, const sigil_card_listing *listing,
                          const volume_pair *pair, size_t which) {
    save_set saves;
    if (!extract_all(vol, listing, &saves)) { fail(id, "extract failed"); return; }
    sigil_segacd_volume built = {0}, again = {0};
    sigil_card_listing *rebuilt = NULL;
    char label[256];
    snprintf(label, sizeof(label), "%s (rebuilt)", id);
    if (!build_volume(vol, &saves, &built) || !build_volume(vol, &saves, &again)) {
        fail(label, "rebuild failed");
    } else if (!written_equal(&built, &again)) {
        fail(label, "two builds from the same saves differ");
    } else if (sigil_segacd_list(&built, &rebuilt) != SIGIL_OK) {
        fail(label, "rebuilt volume did not list");
    } else {
        volume_pair p = *pair;
        p.vol[which] = &built;
        p.listing[which] = rebuilt;
        check_expected_entries(label, id, path, entries, &p);
        if (rebuilt->free_blocks != listing->free_blocks) fail(label, "free blocks differ");
        check_same_layout(label, vol, &built, listing);
        for (size_t i = 0; i < saves.count; i++) {
            if (sigil_segacd_verify(&built, saves.unit[i], saves.len[i]) != SIGIL_OK) fail(label, "a save did not verify");
        }
    }
    sigil_card_listing_free(rebuilt);
    sigil_segacd_volume_free(&built);
    sigil_segacd_volume_free(&again);
    save_set_free(&saves);
}

static uint8_t *sample_bytes(const char *id, const char *path, size_t *len) {
    char full[1024];
    if (corpus_sample_path(PLATFORM, id, path, full, sizeof(full)) != 0) return NULL;
    return corpus_read_file(full, len);
}

static void check_write_back(const char *id, const char *path, const sigil_segacd_volume *vol) {
    size_t orig_len = 0;
    uint8_t *orig = sample_bytes(id, path, &orig_len);
    uint8_t *out = NULL;
    size_t out_len = 0;
    if (!orig || sigil_segacd_volume_write(vol, &out, &out_len) != SIGIL_OK ||
        out_len != orig_len || memcmp(out, orig, orig_len) != 0) {
        fail(id, "written volume differs from the file");
    }
    free(out);
    free(orig);
}

static void check_volume(const char *id, const char *path, const corpus_table *entries) {
    char full[1024];
    if (corpus_sample_path(PLATFORM, id, path, full, sizeof(full)) != 0) return;
    sigil_io *io = sigil_io_open_file(full);
    if (!io) return;
    g_volumes++;

    sigil_segacd_volume vol = {0}, cart = {0};
    sigil_card_listing *listing = NULL, *cart_listing = NULL;
    if (sigil_segacd_volume_load(io, &vol) != SIGIL_OK || sigil_segacd_list(&vol, &listing) != SIGIL_OK) {
        fail(id, "volume did not load");
    } else {
        volume_pair pair = { { &vol, NULL }, { listing, NULL }, 1 };
        if (vol.other_size != 0) {
            if (sigil_segacd_volume_load_cart(io, &cart) != SIGIL_OK || sigil_segacd_list(&cart, &cart_listing) != SIGIL_OK) {
                fail(id, "cart part did not load");
            } else {
                pair.vol[1] = &cart;
                pair.listing[1] = cart_listing;
                pair.count = 2;
            }
        } else if (sigil_segacd_volume_load_cart(io, &cart) != SIGIL_ERR_UNSUPPORTED_FORMAT) {
            fail(id, "a single volume read as a combined file");
        }
        check_expected_entries(id, id, path, entries, &pair);
        for (size_t v = 0; v < pair.count; v++) {
            check_facts(id, v == 1, pair.listing[v]);
            check_rebuild(id, path, entries, pair.vol[v], pair.listing[v], &pair, v);
            check_write_back(id, path, pair.vol[v]);
        }
        sigil_card_listing *public_listing = NULL;
        if (sigil_card_list(io, &public_listing) != SIGIL_OK ||
            public_listing->format != SIGIL_CARD_FORMAT_SEGACD_BRAM ||
            public_listing->entry_count != listing->entry_count) {
            fail(id, "sigil_card_list disagrees with the internal listing");
        }
        sigil_card_listing_free(public_listing);
    }
    sigil_card_listing_free(listing);
    sigil_card_listing_free(cart_listing);
    sigil_segacd_volume_free(&vol);
    sigil_segacd_volume_free(&cart);
    sigil_io_close(io);
}

static bool load_bytes(const uint8_t *data, size_t len, sigil_segacd_volume *vol) {
    mem_ctx m = { data, len };
    sigil_io io = { mem_read, mem_size, NULL, &m };
    return sigil_segacd_volume_load(&io, vol) == SIGIL_OK;
}

static bool load_sample(const char *id, const char *path, sigil_segacd_volume *vol) {
    size_t len = 0;
    uint8_t *bytes = sample_bytes(id, path, &len);
    bool ok = bytes && load_bytes(bytes, len, vol);
    free(bytes);
    return ok;
}

#define MULTI_TITLES "multi-titles-brm", "Multiple titles.brm"
#define DARK_WIZARD  "dark-wizard-brm", "Dark Wizard (USA) cd-bram.brm"
#define LUNAR        "lunar-ecc-brm", "Lunar_The_Silver_Star-internal-memory.brm"
#define POPFUL_CART  "popful-mail-cart-brm", "Popful Mail (USA) (RE)-from-emulator cart.brm"
#define SFCD         "sfcd-internal-plus-cart", "SHINING FORCE CD.brm"

/* The blank samples are formatted volumes; a volume sigil formats matches them. */
static void check_format(void) {
    static const sigil_bram_storage raw = {0};
    static const struct { const char *id; const char *path; size_t size; } BLANKS[] = {
        { "blank-internal", "Empty-mister-save.sav", SEGACD_INTERNAL_SIZE },
        { "blank-cart-brm", "Lunar_The_Silver_Star-ram-cart.brm", SEGACD_MAX_CART_SIZE },
    };
    for (size_t i = 0; i < sizeof(BLANKS) / sizeof(BLANKS[0]); i++) {
        size_t len = 0;
        uint8_t *blank = sample_bytes(BLANKS[i].id, BLANKS[i].path, &len);
        if (!blank) continue;
        sigil_segacd_volume vol = {0};
        if (sigil_segacd_volume_format(&vol, BLANKS[i].size, &raw) != SIGIL_OK ||
            vol.size != len || memcmp(vol.data, blank, len) != 0) {
            fail(BLANKS[i].id, "a formatted volume differs from the blank sample");
        }
        sigil_card_listing *l = NULL;
        if (i == 0 && (sigil_segacd_list(&vol, &l) != SIGIL_OK || l->free_slots != 83)) {
            fail(BLANKS[i].id, "an empty internal volume should take 83 one-block saves");
        }
        sigil_card_listing_free(l);
        sigil_segacd_volume_free(&vol);
        free(blank);
    }
}

static bool extract_named(const sigil_segacd_volume *vol, const char *name, uint8_t **unit, size_t *len) {
    sigil_card_listing *l = NULL;
    bool ok = false;
    if (sigil_segacd_list(vol, &l) == SIGIL_OK) {
        const sigil_card_entry *e = find_entry(l, name);
        if (e) {
            *len = sigil_segacd_unit_size(e->blocks);
            *unit = (uint8_t *)malloc(*len);
            ok = *unit && sigil_segacd_extract(vol, e->first_block, e->blocks, *unit) == SIGIL_OK;
        }
    }
    sigil_card_listing_free(l);
    return ok;
}

static void expect_refused(const char *label, sigil_segacd_volume *vol, const uint8_t *unit, size_t len, int want) {
    uint8_t *before = (uint8_t *)malloc(vol->size);
    if (!before) return;
    memcpy(before, vol->data, vol->size);
    if (sigil_segacd_inject(vol, unit, len) != want) fail(label, "inject was not refused");
    if (memcmp(before, vol->data, vol->size) != 0) fail(label, "a refused inject changed the volume");
    free(before);
}

/* The four copies of each count sit at the format block's 0x10 and 0x18. */
static void expect_counts(const char *label, const sigil_segacd_volume *vol, uint32_t free_blocks, uint32_t files) {
    const uint8_t *f = vol->data + vol->size - SEGACD_BLOCK_SIZE;
    for (unsigned i = 0; i < 4; i++) {
        if (((f[0x10 + 2 * i] << 8) | f[0x11 + 2 * i]) != (int)free_blocks) fail(label, "a free count copy differs");
        if (((f[0x18 + 2 * i] << 8) | f[0x19 + 2 * i]) != (int)files) fail(label, "a file count copy differs");
    }
}

/* Dark Wizard holds two 40-block saves with 44 blocks free; Lunar one 40-block
 * save with 85 free (the counts in VOLUME_FACTS). Adding GA_LUNAR_01 with an
 * even entry count costs its 40 blocks; deleting both Dark Wizard saves leaves
 * a volume shaped like the Lunar sample. */
static void check_inject_and_delete(void) {
    const char *id = "dark-wizard-brm";
    sigil_segacd_volume lunar = {0}, vol = {0};
    uint8_t *ga = NULL, *dw0 = NULL, *dw1 = NULL;
    size_t ga_len = 0, dw0_len = 0, dw1_len = 0;
    if (!load_sample(LUNAR, &lunar) || !load_sample(DARK_WIZARD, &vol) ||
        !extract_named(&lunar, "GA_LUNAR_01", &ga, &ga_len) ||
        !extract_named(&vol, "DW__DATA_00", &dw0, &dw0_len) ||
        !extract_named(&vol, "DW__DATA_01", &dw1, &dw1_len)) {
        goto done;
    }
    if (sigil_segacd_inject(&vol, ga, ga_len) != SIGIL_OK) {
        fail(id, "inject failed");
        goto done;
    }
    expect_counts("dark-wizard-brm inject", &vol, 4, 3);
    if (sigil_segacd_verify(&vol, ga, ga_len) != SIGIL_OK) fail(id, "injected save did not verify");
    if (sigil_segacd_verify(&vol, dw0, dw0_len) != SIGIL_OK || sigil_segacd_verify(&vol, dw1, dw1_len) != SIGIL_OK) {
        fail(id, "an existing save changed");
    }

    sigil_card_listing *l = NULL;
    const sigil_card_entry *e = NULL;
    if (sigil_segacd_list(&vol, &l) != SIGIL_OK || !(e = find_entry(l, "DW__DATA_00")) ||
        sigil_segacd_delete(&vol, e->first_block) != SIGIL_OK) {
        fail(id, "delete failed");
    } else {
        expect_counts("dark-wizard-brm delete", &vol, 44, 2);
        if (sigil_segacd_verify(&vol, dw0, dw0_len) == SIGIL_OK) fail(id, "deleted save still verifies");
        if (sigil_segacd_verify(&vol, dw1, dw1_len) != SIGIL_OK || sigil_segacd_verify(&vol, ga, ga_len) != SIGIL_OK) {
            fail(id, "delete touched another save");
        }
    }
    sigil_card_listing_free(l);
    l = NULL;
    if (sigil_segacd_list(&vol, &l) != SIGIL_OK || !(e = find_entry(l, "DW__DATA_01")) ||
        sigil_segacd_delete(&vol, e->first_block) != SIGIL_OK) {
        fail(id, "second delete failed");
    } else {
        expect_counts("dark-wizard-brm second delete", &vol, 85, 1);
        if (sigil_segacd_verify(&vol, ga, ga_len) != SIGIL_OK) fail(id, "the remaining save changed");
        if (memcmp(vol.data + SEGACD_BLOCK_SIZE, lunar.data + SEGACD_BLOCK_SIZE, (size_t)40 * SEGACD_BLOCK_SIZE) != 0) {
            fail(id, "the remaining save's blocks differ from the Lunar sample's");
        }
    }
    sigil_card_listing_free(l);
done:
    free(ga);
    free(dw0);
    free(dw1);
    sigil_segacd_volume_free(&vol);
    sigil_segacd_volume_free(&lunar);
}

/* SFCD_DAT_01 takes 99 blocks; the multi-titles volume has 24 free. Popful
 * Mail's cart already holds a POPFUL_MAIL. */
static void check_refusals(void) {
    sigil_segacd_volume multi = {0}, cart = {0}, popful = {0};
    uint8_t *sfcd = NULL, *pm = NULL;
    size_t sfcd_len = 0, pm_len = 0;
    char full[1024];
    sigil_io *io = NULL;
    if (corpus_sample_path(PLATFORM, SFCD, full, sizeof(full)) == 0) io = sigil_io_open_file(full);
    bool have_multi = load_sample(MULTI_TITLES, &multi);
    if (io && have_multi && sigil_segacd_volume_load_cart(io, &cart) == SIGIL_OK &&
        extract_named(&cart, "SFCD_DAT_01", &sfcd, &sfcd_len)) {
        expect_refused("multi-titles-brm full", &multi, sfcd, sfcd_len, SIGIL_ERR_NO_SPACE);
    }
    if (have_multi && extract_named(&multi, "POPFUL_MAIL", &pm, &pm_len) && load_sample(POPFUL_CART, &popful)) {
        expect_refused("popful-mail-cart-brm name", &popful, pm, pm_len, SIGIL_ERR_EXISTS);
        uint8_t *bad = (uint8_t *)malloc(pm_len);
        if (bad) {
            memcpy(bad, pm, pm_len);
            expect_refused("truncated unit", &popful, bad, pm_len - 1, SIGIL_ERR_UNSUPPORTED_FORMAT);
            bad[0] = 'X';
            expect_refused("unit magic", &popful, bad, pm_len, SIGIL_ERR_UNSUPPORTED_FORMAT);
        }
        free(bad);
    }
    if (io) sigil_io_close(io);
    free(sfcd);
    free(pm);
    sigil_segacd_volume_free(&multi);
    sigil_segacd_volume_free(&cart);
    sigil_segacd_volume_free(&popful);
}

/* GA_LUNAR_01's directory entry is slot 0, the second half of block 126
 * (0x1F80); its first data block is block 1 (0x40), and it is protected.
 * Byte 8 of a block carries payload bits (bytes 0-5 carry the first CRC,
 * which the second copy would cover). One flipped bit is corrected; bits 7
 * and 6 of one byte are two symbols of one 8-bit code word, which isn't. */
static void check_ecc_damage(void) {
    static const struct { size_t at; uint8_t mask; bool listed; const char *what; } DAMAGE[] = {
        { 0x1F88, 0x80, true,  "a flipped directory bit was not corrected" },
        { 0x0048, 0x80, true,  "a flipped data bit was not corrected" },
        { 0x1F88, 0xC0, false, "a directory block with two errors listed" },
        { 0x0048, 0xC0, false, "a data block with two errors listed" },
    };
    size_t len = 0;
    uint8_t *bytes = sample_bytes(LUNAR, &len);
    if (!bytes) return;
    for (size_t i = 0; i < sizeof(DAMAGE) / sizeof(DAMAGE[0]); i++) {
        uint8_t *copy = (uint8_t *)malloc(len);
        if (!copy) break;
        memcpy(copy, bytes, len);
        copy[DAMAGE[i].at] ^= DAMAGE[i].mask;
        sigil_segacd_volume vol = {0};
        sigil_card_listing *l = NULL;
        if (!load_bytes(copy, len, &vol) || sigil_segacd_list(&vol, &l) != SIGIL_OK) {
            fail("lunar-ecc-brm", "damaged volume did not load");
        } else if (DAMAGE[i].listed) {
            char md5[33] = "";
            if (l->entry_count == 1) entry_md5(&vol, &l->entries[0], md5);
            if (l->corrupt_count != 0 || strcmp(md5, "4a51900acc84326c5b32ded2cc51378f") != 0) fail("lunar-ecc-brm", DAMAGE[i].what);
        } else if (l->entry_count != 0 || l->corrupt_count != 1) {
            fail("lunar-ecc-brm", DAMAGE[i].what);
        }
        sigil_card_listing_free(l);
        sigil_segacd_volume_free(&vol);
        free(copy);
    }
    free(bytes);
}

/* Three to eight flipped bits can defeat both codes, which may then "correct"
 * the block into another code word; the CRCs catch those. Whatever the damage,
 * a block either fails to decode or decodes to its original payload. */
static void check_no_silent_damage(void) {
    size_t len = 0;
    uint8_t *bytes = sample_bytes(LUNAR, &len);
    if (!bytes || len < 2 * SEGACD_BLOCK_SIZE) { free(bytes); return; }
    const uint8_t *block = bytes + SEGACD_BLOCK_SIZE;
    uint8_t want[SEGACD_PAYLOAD_SIZE], got[SEGACD_PAYLOAD_SIZE], damaged[SEGACD_BLOCK_SIZE];
    size_t silent = 0;
    if (!sigil_segacd_decode_block(block, want)) fail("lunar-ecc-brm", "block 1 did not decode");
    uint32_t state = 1;
    for (unsigned trial = 0; trial < 200000; trial++) {
        memcpy(damaged, block, SEGACD_BLOCK_SIZE);
        unsigned flips = 3 + trial % 6;
        for (unsigned f = 0; f < flips; f++) {
            state = state * 1103515245u + 12345u;
            unsigned bit = (state >> 16) % (8 * SEGACD_BLOCK_SIZE);
            damaged[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        }
        if (sigil_segacd_decode_block(damaged, got) && memcmp(got, want, sizeof(want)) != 0) silent++;
    }
    if (silent) fail("lunar-ecc-brm", "damaged blocks decoded to other data");
    free(bytes);
}

/* No expanded sample exists. These build the two expanded forms from the Dark
 * Wizard volume, data in each odd byte, and check each reads and goes back as
 * it came. */
static void check_expanded(const corpus_table *entries) {
    size_t len = 0;
    uint8_t *bytes = sample_bytes(DARK_WIZARD, &len);
    static const int FILLERS[] = { -1, 0x00, 0xFF };
    uint8_t *wide = bytes ? (uint8_t *)malloc(len * 2) : NULL;
    for (size_t f = 0; wide && f < sizeof(FILLERS) / sizeof(FILLERS[0]); f++) {
        int filler = FILLERS[f];
        for (size_t i = 0; i < len; i++) {
            wide[2 * i] = filler < 0 ? bytes[i] : (uint8_t)filler;
            wide[2 * i + 1] = bytes[i];
        }
        sigil_segacd_volume vol = {0};
        sigil_card_listing *l = NULL;
        uint8_t *out = NULL;
        size_t out_len = 0;
        if (!load_bytes(wide, len * 2, &vol) || !vol.storage.expanded || vol.storage.filler != filler ||
            sigil_segacd_list(&vol, &l) != SIGIL_OK) {
            fail("dark-wizard-brm expanded", "expanded volume did not load");
        } else {
            volume_pair p = { { &vol, NULL }, { l, NULL }, 1 };
            check_expected_entries("dark-wizard-brm expanded", DARK_WIZARD, entries, &p);
            if (sigil_segacd_volume_write(&vol, &out, &out_len) != SIGIL_OK || out_len != len * 2 ||
                memcmp(out, wide, out_len) != 0) {
                fail("dark-wizard-brm expanded", "expanded volume did not go back as it came");
            }
        }
        free(out);
        sigil_card_listing_free(l);
        sigil_segacd_volume_free(&vol);
    }
    free(wide);
    free(bytes);
}

static void expect_not_volume(const char *label, const uint8_t *data, size_t len) {
    mem_ctx m = { data, len };
    sigil_io io = { mem_read, mem_size, NULL, &m };
    sigil_segacd_volume vol = {0};
    if (sigil_segacd_volume_load(&io, &vol) != SIGIL_ERR_UNSUPPORTED_FORMAT) fail(label, "read as a volume");
    sigil_segacd_volume_free(&vol);
}

/* A volume cut short, one of an odd size, one without its signature and one
 * whose file count copies disagree are not volumes sigil reads. */
static void check_not_volumes(void) {
    size_t len = 0;
    uint8_t *bytes = sample_bytes(DARK_WIZARD, &len);
    if (!bytes) return;
    expect_not_volume("a volume cut short", bytes, len - SEGACD_BLOCK_SIZE);
    expect_not_volume("odd size", bytes, len - 1);
    uint8_t *copy = (uint8_t *)malloc(len);
    if (copy) {
        memcpy(copy, bytes, len);
        copy[len - 0x20] = 'X';
        expect_not_volume("no signature", copy, len);
        memcpy(copy, bytes, len);
        copy[len - 0x28 + 1] = 0x05;
        copy[len - 0x28 + 3] = 0x06;
        expect_not_volume("disagreeing file counts", copy, len);
    }
    free(copy);
    free(bytes);
}

int main(void) {
    char path[1024];
    corpus_table manifest, entries;
    if (corpus_platform_path(PLATFORM, "manifest.tsv", path, sizeof(path)) != 0 ||
        corpus_load(path, &manifest) != 0) {
        fprintf(stderr, "SKIP: no segacd manifest\n");
        return TEST_SKIP;
    }
    if (corpus_platform_path(PLATFORM, "entries.tsv", path, sizeof(path)) != 0 ||
        corpus_load(path, &entries) != 0) {
        corpus_free(&manifest);
        fprintf(stderr, "SKIP: no segacd entries\n");
        return TEST_SKIP;
    }

    for (size_t r = 0; r < manifest.nrows; r++) {
        const char *kind = corpus_get(&manifest, r, "kind");
        const char *id = corpus_get(&manifest, r, "id");
        const char *file = corpus_get(&manifest, r, "path");
        if (kind && id && file && strcmp(kind, "volume") == 0) check_volume(id, file, &entries);
    }
    check_format();
    check_inject_and_delete();
    check_refusals();
    check_ecc_damage();
    check_no_silent_damage();
    check_expanded(&entries);
    check_not_volumes();
    int missing = g_volumes ? corpus_count_missing(&manifest, PLATFORM) : 0;
    corpus_free(&manifest);
    corpus_free(&entries);

    printf("segacd volumes: %d checked, %d failures\n", g_volumes, g_fails);
    if (!g_volumes) return TEST_SKIP;
    return corpus_exit(g_fails + missing);
}
