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
#define ENTRY_START_AT 12u   /* a directory entry's start block, big-endian; an odd slot fills the payload's first half */

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
        bool zeroed = true;
        for (size_t i = (size_t)41 * SEGACD_BLOCK_SIZE; i < (size_t)121 * SEGACD_BLOCK_SIZE && zeroed; i++) zeroed = vol.data[i] == 0;
        if (!zeroed) fail(id, "the blocks the deleted saves freed still hold their data");
    }
    sigil_card_listing_free(l);
done:
    free(ga);
    free(dw0);
    free(dw1);
    sigil_segacd_volume_free(&vol);
    sigil_segacd_volume_free(&lunar);
}

/* The first `blocks` blocks of `unit` as a save named `name`. Each block is
 * its own ECC block, so a prefix is a valid save of that many blocks. */
/* An unprotected unit of `blocks` zero blocks under `name`, with `unit`'s
 * header. */
static uint8_t *zero_unit(const uint8_t *unit, uint32_t blocks, const char *name, size_t *len) {
    *len = sigil_segacd_unit_size(blocks);
    uint8_t *out = (uint8_t *)calloc(1, *len);
    if (!out) return NULL;
    memcpy(out, unit, SEGACD_UNIT_HEADER_SIZE);
    memset(out + 0x04, '_', 11);
    memcpy(out + 0x04, name, strlen(name));
    out[0x0F] = 0;
    out[0x10] = (uint8_t)(blocks >> 8);
    out[0x11] = (uint8_t)blocks;
    return out;
}

static uint8_t *unit_slice(const uint8_t *unit, uint32_t blocks, const char *name, size_t *len) {
    *len = sigil_segacd_unit_size(blocks);
    uint8_t *out = (uint8_t *)malloc(*len);
    if (!out) return NULL;
    memcpy(out, unit, *len);
    memset(out + 0x04, '_', 11);
    memcpy(out + 0x04, name, strlen(name));
    out[0x10] = (uint8_t)(blocks >> 8);
    out[0x11] = (uint8_t)blocks;
    return out;
}

/* A save fits when its blocks and the block the BIOS then holds are free, to
 * the block: three 40-block saves leave 4 free with an odd file count, so a
 * 3-block save (and the hold its even count brings) fits exactly and a
 * 4-block one is refused untouched. Every save still verifies after the
 * exact fit. */
static void check_exact_fit(void) {
    size_t blank_len = 0;
    uint8_t *blank = sample_bytes("blank-internal", "Empty-mister-save.sav", &blank_len);
    sigil_segacd_volume lunar = {0}, vol = {0};
    uint8_t *ga = NULL, *fill[3] = { NULL, NULL, NULL }, *three = NULL, *four = NULL;
    size_t ga_len = 0, fill_len = 0, three_len = 0, four_len = 0;
    static const char *const NAMES[3] = { "FILL_A", "FILL_B", "FILL_C" };
    if (!blank || !load_bytes(blank, blank_len, &vol) || !load_sample(LUNAR, &lunar) ||
        !extract_named(&lunar, "GA_LUNAR_01", &ga, &ga_len)) {
        fail("exact fit", "setup failed");
        goto done;
    }
    for (int i = 0; i < 3; i++) {
        fill[i] = unit_slice(ga, 40, NAMES[i], &fill_len);
        if (!fill[i] || sigil_segacd_inject(&vol, fill[i], fill_len) != SIGIL_OK) { fail("exact fit", "filling failed"); goto done; }
    }
    expect_counts("exact fit filled", &vol, 4, 3);
    four = unit_slice(ga, 4, "FOUR", &four_len);
    three = unit_slice(ga, 3, "THREE", &three_len);
    if (!four || !three) { fail("exact fit", "setup failed"); goto done; }
    expect_refused("exact fit one block over", &vol, four, four_len, SIGIL_ERR_NO_SPACE);
    if (sigil_segacd_cost(&vol, four, four_len) != 5) fail("exact fit", "an odd file count's directory block isn't in the cost");
    size_t ten_len = 0;
    uint8_t *ten = unit_slice(ga, 10, "TEN", &ten_len);
    if (ten) {
        uint8_t saved[8];
        uint8_t *count = vol.data + vol.size - SEGACD_BLOCK_SIZE + 0x10;
        memcpy(saved, count, 8);
        for (int i = 0; i < 4; i++) { count[2 * i] = 0; count[2 * i + 1] = 50; }
        expect_refused("exact fit, a stored free count that overstates the space", &vol, ten, ten_len, SIGIL_ERR_NO_SPACE);
        memcpy(count, saved, 8);
        free(ten);
    }
    if (sigil_segacd_inject(&vol, three, three_len) != SIGIL_OK) {
        fail("exact fit", "a save that fits to the block was refused");
    } else {
        expect_counts("exact fit", &vol, 0, 4);
        if (sigil_segacd_cost(&vol, four, four_len) != 4) fail("exact fit", "an even file count added a directory block to the cost");
        for (int i = 0; i < 3; i++) {
            if (sigil_segacd_verify(&vol, fill[i], fill_len) != SIGIL_OK) fail("exact fit", "a save changed when the volume filled");
        }
        if (sigil_segacd_verify(&vol, three, three_len) != SIGIL_OK) fail("exact fit", "the fitting save didn't verify");
    }
done:
    for (int i = 0; i < 3; i++) free(fill[i]);
    free(three);
    free(four);
    free(ga);
    free(blank);
    sigil_segacd_volume_free(&vol);
    sigil_segacd_volume_free(&lunar);
}

/* The BIOS writes each count four times and reads it when three agree: a
 * volume whose first copy is damaged still reads. */
/* The directory slot `slot` of `vol` as its 16 bytes, through the codec. */
static uint8_t *slot_payload(sigil_segacd_volume *vol, uint32_t slot, uint8_t payload[SEGACD_PAYLOAD_SIZE]) {
    uint8_t *block = vol->data + vol->size - (size_t)(2 + slot / 2) * SEGACD_BLOCK_SIZE;
    if (!sigil_segacd_decode_block(block, payload)) return NULL;
    return block;
}

/* The BIOS keeps saves back to back from block 1 and its directory exact:
 * a save starting a block late is refused for inject; a save ending at the
 * last data block reads; deleting the last entry in an odd slot clears its
 * half of the block; a protected unit that doesn't decode isn't injected. */
static void check_directory_rules(void) {
    sigil_segacd_volume vol = {0}, lunar = {0};
    uint8_t *ga = NULL, *fill = NULL, *rest = NULL;
    size_t ga_len = 0, fill_len = 0, rest_len = 0;
    uint8_t payload[SEGACD_PAYLOAD_SIZE];
    if (!load_sample(LUNAR, &lunar) || !extract_named(&lunar, "GA_LUNAR_01", &ga, &ga_len)) {
        fail("directory rules", "setup failed");
        goto done;
    }

    /* With an even file count the BIOS holds the block before the directory
     * for the next entry: a save running into it is corrupt, since sigil
     * couldn't put the volume back as it is after deleting another save. */
    size_t held_len = 0;
    uint8_t *held = sample_bytes("blank-internal", "Empty-mister-save.sav", &held_len);
    if (held && load_bytes(held, held_len, &vol)) {
        size_t one_len = 0, last_len = 0;
        uint8_t *one = unit_slice(ga, 1, "AAAA", &one_len), *last = NULL;
        sigil_card_listing *l = NULL;
        bool ok = one && sigil_segacd_inject(&vol, one, one_len) == SIGIL_OK && sigil_segacd_list(&vol, &l) == SIGIL_OK;
        uint32_t room = ok && l->free_blocks ? l->free_blocks - 1 : 0;
        sigil_card_listing_free(l);
        l = NULL;
        last = room ? zero_unit(ga, room, "BBBB", &last_len) : NULL;
        uint8_t *block = last && sigil_segacd_inject(&vol, last, last_len) == SIGIL_OK ? slot_payload(&vol, 1, payload) : NULL;
        if (!block) {
            fail("directory rules", "setup failed");
        } else {
            uint16_t blocks = (uint16_t)(payload[ENTRY_START_AT + 2] << 8 | payload[ENTRY_START_AT + 3]);
            payload[ENTRY_START_AT + 2] = (uint8_t)((blocks + 1) >> 8);
            payload[ENTRY_START_AT + 3] = (uint8_t)(blocks + 1);
            sigil_segacd_encode_block(payload, block);
            if (sigil_segacd_list(&vol, &l) != SIGIL_OK || l->entry_count != 1 || !find_entry(l, "AAAA_______") ||
                l->corrupt_entry_count != 1 || strncmp(l->corrupt_entries[0].name, "BBBB", 4) != 0) {
                fail("directory rules", "a save running into the block the BIOS holds listed");
            }
            sigil_card_listing_free(l);
        }
        free(one);
        free(last);
        sigil_segacd_volume_free(&vol);
    }
    free(held);

    /* Block 0 holds no save data: an entry starting there is corrupt. */
    if (load_sample(DARK_WIZARD, &vol)) {
        uint8_t *block = slot_payload(&vol, 0, payload);
        sigil_card_listing *l = NULL;
        if (!block) {
            fail("directory rules", "setup failed");
        } else {
            payload[16 + ENTRY_START_AT] = 0;
            payload[16 + ENTRY_START_AT + 1] = 0;
            sigil_segacd_encode_block(payload, block);
            if (sigil_segacd_list(&vol, &l) != SIGIL_OK || l->corrupt_entry_count != 1) {
                fail("directory rules", "an entry starting at block 0 listed");
            }
        }
        sigil_card_listing_free(l);
        sigil_segacd_volume_free(&vol);
    }

    if (load_sample(DARK_WIZARD, &vol)) {
        uint8_t *block = slot_payload(&vol, 1, payload);
        if (!block) {
            fail("directory rules", "setup failed");
        } else {
            payload[ENTRY_START_AT + 1]++;
            sigil_segacd_encode_block(payload, block);
            fill = unit_slice(ga, 1, "ONE", &fill_len);
            if (fill) expect_refused("a save a block late", &vol, fill, fill_len, SIGIL_ERR_UNSUPPORTED_FORMAT);
            free(fill);
            fill = NULL;
        }
        sigil_segacd_volume_free(&vol);
    }

    if (load_sample(DARK_WIZARD, &vol)) {
        sigil_card_listing *l = NULL;
        const sigil_card_entry *e = NULL;
        uint8_t *block = NULL;
        if (sigil_segacd_list(&vol, &l) != SIGIL_OK || !(e = find_entry(l, "DW__DATA_01")) ||
            sigil_segacd_delete(&vol, e->first_block) != SIGIL_OK || !(block = slot_payload(&vol, 1, payload))) {
            fail("directory rules", "deleting the last entry failed");
        } else {
            for (uint32_t i = 0; i < 16; i++) {
                if (payload[i]) { fail("directory rules", "a deleted entry in an odd slot stayed in its block"); break; }
            }
        }
        sigil_card_listing_free(l);
        sigil_segacd_volume_free(&vol);
    }

    size_t blank_len = 0;
    uint8_t *blank = sample_bytes("blank-internal", "Empty-mister-save.sav", &blank_len);
    if (blank && load_bytes(blank, blank_len, &vol)) {
        fill = unit_slice(ga, 40, "FILL_A", &fill_len);
        uint8_t *second = unit_slice(ga, 40, "FILL_B", &fill_len);
        sigil_card_listing *l = NULL;
        bool ok = fill && second && sigil_segacd_inject(&vol, fill, fill_len) == SIGIL_OK &&
                  sigil_segacd_inject(&vol, second, fill_len) == SIGIL_OK && sigil_segacd_list(&vol, &l) == SIGIL_OK;
        uint32_t room = ok ? l->free_blocks : 0;
        sigil_card_listing_free(l);
        l = NULL;
        for (int pair = 0; ok && room > 40; pair++) {
            char a[8], b[8];
            snprintf(a, sizeof(a), "P%dA", pair);
            snprintf(b, sizeof(b), "P%dB", pair);
            size_t one_len = 0;
            uint8_t *x = unit_slice(ga, 1, a, &one_len), *y = unit_slice(ga, 1, b, &one_len);
            ok = x && y && sigil_segacd_inject(&vol, x, one_len) == SIGIL_OK &&
                 sigil_segacd_inject(&vol, y, one_len) == SIGIL_OK && sigil_segacd_list(&vol, &l) == SIGIL_OK;
            room = ok ? l->free_blocks : 0;
            sigil_card_listing_free(l);
            l = NULL;
            free(x);
            free(y);
        }
        rest = ok && room ? unit_slice(ga, room, "REST", &rest_len) : NULL;
        if (!rest || sigil_segacd_inject(&vol, rest, rest_len) != SIGIL_OK) {
            fail("directory rules", "a save of exactly the free blocks with an even file count was refused");
        } else if (sigil_segacd_list(&vol, &l) != SIGIL_OK || l->free_blocks != 0 || l->corrupt_count != 0 ||
                   !find_entry(l, "REST_______")) {
            fail("directory rules", "a save ending at the last data block didn't list");
        }
        sigil_card_listing_free(l);
        free(second);
        sigil_segacd_volume_free(&vol);
    }
    free(blank);

    if (load_sample(DARK_WIZARD, &vol)) {
        uint8_t *bad = (uint8_t *)malloc(ga_len);
        if (bad) {
            memcpy(bad, ga, ga_len);
            bad[sigil_segacd_unit_size(0) + 8] ^= 0xC0;
            expect_refused("a protected unit that doesn't decode", &vol, bad, ga_len, SIGIL_ERR_UNSUPPORTED_FORMAT);
        }
        free(bad);
        sigil_segacd_volume_free(&vol);
    }
done:
    free(rest);
    free(fill);
    free(ga);
    sigil_segacd_volume_free(&lunar);
}

static void check_count_copies(void) {
    size_t len = 0;
    uint8_t *bytes = sample_bytes("dark-wizard-brm", "Dark Wizard (USA) cd-bram.brm", &len);
    sigil_segacd_volume vol = {0};
    if (!bytes) { fail("count copies", "setup failed"); return; }
    bytes[len - SEGACD_BLOCK_SIZE + 0x18] ^= 0xFF;
    sigil_card_listing *l = NULL;
    if (!load_bytes(bytes, len, &vol) || sigil_segacd_list(&vol, &l) != SIGIL_OK || l->entry_count != 2) {
        fail("count copies", "a volume with one damaged count copy didn't read");
    }
    sigil_card_listing_free(l);
    sigil_segacd_volume_free(&vol);
    free(bytes);
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
    static const struct { size_t at; uint8_t mask; bool listed; bool named; const char *what; } DAMAGE[] = {
        { 0x1F88, 0x80, true,  false, "a flipped directory bit was not corrected" },
        { 0x0048, 0x80, true,  false, "a flipped data bit was not corrected" },
        { 0x1F88, 0xC0, false, false, "a directory block with two errors listed" },
        { 0x0048, 0xC0, false, true,  "a data block with two errors listed" },
    };
    size_t len = 0;
    uint8_t *bytes = sample_bytes(LUNAR, &len);
    if (!bytes) return;
    char name[SIGIL_CARD_NAME_MAX] = "";
    sigil_segacd_volume whole = {0};
    sigil_card_listing *wl = NULL;
    if (load_bytes(bytes, len, &whole) && sigil_segacd_list(&whole, &wl) == SIGIL_OK && wl->entry_count == 1) {
        snprintf(name, sizeof(name), "%s", wl->entries[0].name);
    }
    sigil_card_listing_free(wl);
    sigil_segacd_volume_free(&whole);
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
        } else if (DAMAGE[i].named != (l->corrupt_entry_count == 1) ||
                   (DAMAGE[i].named && strcmp(l->corrupt_entries[0].name, name) != 0)) {
            fail("lunar-ecc-brm", "a corrupt save the directory still names isn't named, or one it doesn't is");
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
        memcpy(copy, bytes, len);
        for (int i = 0; i < 4; i++) {
            copy[len - SEGACD_BLOCK_SIZE + 0x18 + 2 * i] = 0x01;
            copy[len - SEGACD_BLOCK_SIZE + 0x19 + 2 * i] = 0x00;
        }
        sigil_segacd_volume vol = {0};
        sigil_card_listing *l = NULL;
        if (load_bytes(copy, len, &vol) && sigil_segacd_list(&vol, &l) == SIGIL_OK) {
            fail("file counts past the volume", "a volume counting more files than it holds listed");
        }
        sigil_card_listing_free(l);
        sigil_segacd_volume_free(&vol);
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
    check_exact_fit();
    check_count_copies();
    check_directory_rules();
    check_expanded(&entries);
    check_not_volumes();
    int missing = g_volumes ? corpus_count_missing(&manifest, PLATFORM) : 0;
    corpus_free(&manifest);
    corpus_free(&entries);

    printf("segacd volumes: %d checked, %d failures\n", g_volumes, g_fails);
    if (!g_volumes) return TEST_SKIP;
    return corpus_exit(g_fails + missing);
}
