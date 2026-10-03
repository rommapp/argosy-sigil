// SPDX-License-Identifier: MPL-2.0
#include "save_corpus.h"
#include "mem_root.h"
#include "card_ps1.h"
#include <stdbool.h>

#define TEST_SKIP 77

static int g_fails = 0;
static int g_cards = 0;

static void fail(const char *id, const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", id, what);
    g_fails++;
}

/* The manifests store the product code as it appears in the name; the
 * listing reports it with the separator normalized to '-'. */
static void expected_owner(const char *raw, char out[SIGIL_CARD_OWNER_MAX]) {
    out[0] = '\0';
    if (strcmp(raw, "-") == 0 || strlen(raw) != 10) return;
    memcpy(out, raw, 11);
    out[4] = '-';
}

static void entry_md5(const uint8_t *image, const sigil_card_entry *e, char out[33]) {
    out[0] = '\0';
    size_t len = (size_t)e->blocks * PS1_BLOCK_SIZE;
    uint8_t *data = (uint8_t *)malloc(len);
    if (!data) return;
    if (sigil_ps1_entry_data(image, e->first_block, e->blocks, data) == SIGIL_OK) {
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

static void check_expected_entries(const char *where, const char *id, const char *path,
                                   const corpus_table *entries,
                                   const sigil_card_listing *listing, const uint8_t *image) {
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

        char owner[SIGIL_CARD_OWNER_MAX];
        expected_owner(corpus_get(entries, r, "owner_id"), owner);
        if (strcmp(owner, e->owner_id) != 0) fail(label, "owner_id differs");
        if (strtoul(corpus_get(entries, r, "blocks"), NULL, 10) != e->blocks) fail(label, "block count differs");

        char md5[33];
        entry_md5(image, e, md5);
        if (strcmp(md5, corpus_get(entries, r, "data_md5")) != 0) fail(label, "data md5 differs");
    }
    if (expected != listing->entry_count) fail(where, "listing holds entries entries.tsv doesn't");
}

typedef struct {
    const char *id;
    uint32_t    free_slots;
    uint32_t    corrupt_count;
} card_fact;

/* Read from each card's directory frames by hex dump, and matching the
 * manifest notes. */
static const card_fact CARD_FACTS[] = {
    { "blank-card",           15, 0 },
    { "all-deleted-mcr",      15, 0 },
    { "xenogears-full-mcd",    0, 0 },
    { "ff8-cheat-entries-mcd", 0, 0 },
    { "megaman-bad-link-mcd", 11, 1 },
};

static void check_facts(const char *id, const sigil_card_listing *listing) {
    for (size_t i = 0; i < sizeof(CARD_FACTS) / sizeof(CARD_FACTS[0]); i++) {
        if (strcmp(CARD_FACTS[i].id, id) != 0) continue;
        if (listing->free_slots != CARD_FACTS[i].free_slots) fail(id, "free slot count differs");
        if (listing->corrupt_count != CARD_FACTS[i].corrupt_count) fail(id, "corrupt count differs");
    }
}

/* Every save on the card, in directory order, as .mcs buffers. */
typedef struct {
    uint8_t *mcs[PS1_DATA_BLOCKS];
    size_t   len[PS1_DATA_BLOCKS];
    size_t   count;
} save_set;

static void save_set_free(save_set *s) {
    for (size_t i = 0; i < s->count; i++) free(s->mcs[i]);
    s->count = 0;
}

/* A .mcs header is a standalone directory frame: no link to follow, and the
 * XOR checksum over its first 127 bytes in the last. */
static bool mcs_header_valid(const uint8_t *mcs) {
    if (mcs[8] != 0xFF || mcs[9] != 0xFF) return false;
    uint8_t x = 0;
    for (size_t i = 0; i < PS1_FRAME_SIZE - 1; i++) x ^= mcs[i];
    return x == mcs[PS1_FRAME_SIZE - 1];
}

static bool extract_all(const uint8_t *image, const sigil_card_listing *l, save_set *out) {
    out->count = 0;
    for (size_t i = 0; i < l->entry_count; i++) {
        size_t len = sigil_ps1_mcs_size(l->entries[i].blocks);
        uint8_t *mcs = (uint8_t *)malloc(len);
        if (!mcs || sigil_ps1_extract(image, l->entries[i].first_block, l->entries[i].blocks, mcs) != SIGIL_OK) {
            free(mcs);
            save_set_free(out);
            return false;
        }
        out->mcs[out->count] = mcs;
        out->len[out->count++] = len;
    }
    return true;
}

static bool build_card(const save_set *saves, uint8_t *image) {
    sigil_ps1_format(image);
    for (size_t i = 0; i < saves->count; i++) {
        if (sigil_ps1_inject(image, saves->mcs[i], saves->len[i]) != SIGIL_OK) return false;
    }
    return true;
}

/* Rebuilding a card from its own saves keeps every save's bytes, builds the
 * same card twice, and leaves each save verifiable. */
static void check_rebuild(const char *id, const char *path, const corpus_table *entries,
                          const uint8_t *image, const sigil_card_listing *listing) {
    save_set saves;
    if (!extract_all(image, listing, &saves)) { fail(id, "extract failed"); return; }
    for (size_t i = 0; i < saves.count; i++) {
        if (!mcs_header_valid(saves.mcs[i])) fail(id, "an extracted .mcs header keeps a link or a stale checksum");
    }

    uint8_t *built = (uint8_t *)malloc(PS1_CARD_SIZE);
    uint8_t *again = (uint8_t *)malloc(PS1_CARD_SIZE);
    sigil_card_listing *rebuilt = NULL;
    if (!built || !again || !build_card(&saves, built) || !build_card(&saves, again)) {
        fail(id, "rebuild failed");
    } else if (memcmp(built, again, PS1_CARD_SIZE) != 0) {
        fail(id, "two builds from the same saves differ");
    } else if (sigil_ps1_card_list(built, SIGIL_CARD_FORMAT_PS1_RAW, &rebuilt) != SIGIL_OK) {
        fail(id, "rebuilt card did not list");
    } else {
        char label[256];
        snprintf(label, sizeof(label), "%s (rebuilt)", id);
        check_expected_entries(label, id, path, entries, rebuilt, built);
        for (size_t i = 0; i < saves.count; i++) {
            if (sigil_ps1_verify(built, saves.mcs[i], saves.len[i]) != SIGIL_OK) fail(label, "a save did not verify");
        }
    }
    sigil_card_listing_free(rebuilt);
    free(built);
    free(again);
    save_set_free(&saves);
}

static void check_card(const char *id, const char *path, const corpus_table *entries) {
    char full[1024];
    if (corpus_sample_path("psx", id, path, full, sizeof(full)) != 0) return;
    sigil_io *io = sigil_io_open_file(full);
    if (!io) return;
    g_cards++;

    uint8_t *image = (uint8_t *)malloc(PS1_CARD_SIZE);
    int format = 0;
    sigil_card_listing *listing = NULL;
    if (!image || sigil_ps1_card_load(io, image, &format) != SIGIL_OK ||
        sigil_ps1_card_list(image, format, &listing) != SIGIL_OK) {
        fail(id, "card did not load");
    } else {
        check_expected_entries(id, id, path, entries, listing, image);
        check_facts(id, listing);
        check_rebuild(id, path, entries, image, listing);
        sigil_card_listing *public_listing = NULL;
        bool same = sigil_card_list(io, &public_listing) == SIGIL_OK &&
                    public_listing->entry_count == listing->entry_count &&
                    public_listing->free_blocks == listing->free_blocks &&
                    public_listing->corrupt_count == listing->corrupt_count;
        for (size_t i = 0; same && i < listing->entry_count; i++) {
            const sigil_card_entry *a = &public_listing->entries[i], *b = &listing->entries[i];
            same = strcmp(a->name, b->name) == 0 && strcmp(a->owner_id, b->owner_id) == 0 && a->blocks == b->blocks &&
                   a->first_block == b->first_block;
        }
        if (!same) fail(id, "sigil_card_list disagrees with the internal listing");
        sigil_card_listing_free(public_listing);
    }
    sigil_card_listing_free(listing);
    free(image);
    sigil_io_close(io);
}

static void check_not_card(const char *id, const char *path) {
    char full[1024];
    if (corpus_sample_path("psx", id, path, full, sizeof(full)) != 0) return;
    sigil_io *io = sigil_io_open_file(full);
    if (!io) return;
    sigil_card_listing *listing = NULL;
    if (sigil_card_list(io, &listing) != SIGIL_ERR_UNSUPPORTED_FORMAT) {
        fail(id, "a single save read as a card");
    }
    sigil_card_listing_free(listing);
    sigil_io_close(io);
}

/* Loads a sample card by id into `image`. False when the sample is absent. */
static bool load_sample(const corpus_table *manifest, const char *id, uint8_t *image) {
    for (size_t r = 0; r < manifest->nrows; r++) {
        if (strcmp(corpus_get(manifest, r, "id"), id) != 0) continue;
        char full[1024];
        if (corpus_sample_path("psx", id, corpus_get(manifest, r, "path"), full, sizeof(full)) != 0) return false;
        sigil_io *io = sigil_io_open_file(full);
        if (!io) return false;
        int format = 0;
        int rc = sigil_ps1_card_load(io, image, &format);
        sigil_io_close(io);
        return rc == SIGIL_OK;
    }
    return false;
}

static uint8_t *load_sample_bytes(const corpus_table *manifest, const char *id, size_t *len) {
    for (size_t r = 0; r < manifest->nrows; r++) {
        if (strcmp(corpus_get(manifest, r, "id"), id) != 0) continue;
        char full[1024];
        if (corpus_sample_path("psx", id, corpus_get(manifest, r, "path"), full, sizeof(full)) != 0) return NULL;
        return corpus_read_file(full, len);
    }
    return NULL;
}

/* An emulator formatted the blank-card sample; a card sigil formats matches it. */
static void check_format_matches_emulator(const corpus_table *manifest) {
    size_t len = 0;
    uint8_t *blank = load_sample_bytes(manifest, "blank-card", &len);
    if (!blank) return;
    uint8_t *image = (uint8_t *)malloc(PS1_CARD_SIZE);
    if (image) {
        sigil_ps1_format(image);
        if (len != PS1_CARD_SIZE || memcmp(image, blank, PS1_CARD_SIZE) != 0) {
            fail("blank-card", "a formatted card differs from the emulator's blank card");
        }
    }
    free(image);
    free(blank);
}

/* A .mcs written by another tool goes onto a card and comes back byte for byte. */
static void check_mcs_round_trip(const corpus_table *manifest) {
    size_t len = 0;
    uint8_t *mcs = load_sample_bytes(manifest, "bugs-bunny-mcs", &len);
    if (!mcs) return;
    uint8_t *image = (uint8_t *)malloc(PS1_CARD_SIZE);
    uint8_t *back = (uint8_t *)malloc(len);
    sigil_card_listing *l = NULL;
    if (image && back) {
        sigil_ps1_format(image);
        if (sigil_ps1_inject(image, mcs, len) != SIGIL_OK ||
            sigil_ps1_card_list(image, SIGIL_CARD_FORMAT_PS1_RAW, &l) != SIGIL_OK || l->entry_count != 1 ||
            sigil_ps1_extract(image, l->entries[0].first_block, l->entries[0].blocks, back) != SIGIL_OK) {
            fail("bugs-bunny-mcs", "inject or extract failed");
        } else if (memcmp(back, mcs, len) != 0) {
            fail("bugs-bunny-mcs", "the save changed on its way through a card");
        } else if (strcmp(l->entries[0].owner_id, "SLES-01726") != 0) {
            fail("bugs-bunny-mcs", "owner_id differs");
        }
    }
    sigil_card_listing_free(l);
    free(back);
    free(image);
    free(mcs);
}

/* Injecting into a card that already holds saves leaves them untouched, even
 * when the free blocks it takes are scattered. */
static void check_inject_keeps_other_saves(const corpus_table *manifest) {
    size_t mcs_len = 0;
    uint8_t *mcs = load_sample_bytes(manifest, "bugs-bunny-mcs", &mcs_len);
    uint8_t *image = (uint8_t *)malloc(PS1_CARD_SIZE);
    if (!mcs || !image || !load_sample(manifest, "gran-turismo-gme", image)) {
        free(mcs);
        free(image);
        return;
    }
    sigil_card_listing *before = NULL, *after = NULL;
    save_set kept = {0};
    if (sigil_ps1_card_list(image, SIGIL_CARD_FORMAT_PS1_RAW, &before) != SIGIL_OK ||
        !extract_all(image, before, &kept) ||
        sigil_ps1_inject(image, mcs, mcs_len) != SIGIL_OK ||
        sigil_ps1_card_list(image, SIGIL_CARD_FORMAT_PS1_RAW, &after) != SIGIL_OK) {
        fail("gran-turismo-gme", "inject failed");
    } else {
        if (after->entry_count != before->entry_count + 1) fail("gran-turismo-gme", "entry count after inject");
        if (after->free_slots + 1 != before->free_slots) fail("gran-turismo-gme", "free slots after inject");
        for (size_t i = 0; i < kept.count; i++) {
            if (sigil_ps1_verify(image, kept.mcs[i], kept.len[i]) != SIGIL_OK) {
                fail("gran-turismo-gme", "an existing save changed");
            }
        }
        if (sigil_ps1_verify(image, mcs, mcs_len) != SIGIL_OK) fail("gran-turismo-gme", "injected save did not verify");
        if (sigil_ps1_inject(image, mcs, mcs_len) != SIGIL_ERR_EXISTS) fail("gran-turismo-gme", "a second copy of a save went in");

        const sigil_card_entry *gt = find_entry(after, "BASCUS-94194GT");
        if (!gt || sigil_ps1_delete(image, gt->first_block) != SIGIL_OK) {
            fail("gran-turismo-gme", "delete failed");
        } else {
            sigil_card_listing *deleted = NULL;
            if (sigil_ps1_card_list(image, SIGIL_CARD_FORMAT_PS1_RAW, &deleted) != SIGIL_OK ||
                find_entry(deleted, "BASCUS-94194GT") ||
                deleted->free_slots != after->free_slots + gt->blocks) {
                fail("gran-turismo-gme", "delete did not free the save's blocks");
            }
            if (sigil_ps1_verify(image, mcs, mcs_len) != SIGIL_OK) fail("gran-turismo-gme", "delete touched another save");
            sigil_card_listing_free(deleted);
        }
    }
    save_set_free(&kept);
    sigil_card_listing_free(before);
    sigil_card_listing_free(after);
    free(image);
    free(mcs);
}

/* A save that doesn't fit is refused before anything is written. */
static void check_full_card_refuses(const corpus_table *manifest) {
    size_t mcs_len = 0;
    uint8_t *mcs = load_sample_bytes(manifest, "bugs-bunny-mcs", &mcs_len);
    uint8_t *image = (uint8_t *)malloc(PS1_CARD_SIZE);
    uint8_t *copy = (uint8_t *)malloc(PS1_CARD_SIZE);
    if (mcs && image && copy && load_sample(manifest, "xenogears-full-mcd", image)) {
        memcpy(copy, image, PS1_CARD_SIZE);
        if (sigil_ps1_inject(image, mcs, mcs_len) != SIGIL_ERR_NO_SPACE) fail("xenogears-full-mcd", "full card took a save");
        if (memcmp(copy, image, PS1_CARD_SIZE) != 0) fail("xenogears-full-mcd", "a refused inject changed the card");
    }
    free(copy);
    free(image);
    free(mcs);
}

/* Points block `block`'s directory frame at `link` and reseals it. */
static void set_link(uint8_t *image, uint32_t block, uint16_t link) {
    uint8_t *f = image + (size_t)block * PS1_FRAME_SIZE;
    f[8] = (uint8_t)link;
    f[9] = (uint8_t)(link >> 8);
    uint8_t x = 0;
    for (size_t i = 0; i < PS1_FRAME_SIZE - 1; i++) x ^= f[i];
    f[PS1_FRAME_SIZE - 1] = x;
}

/* A fresh card holding the first save of two blocks or more found on the
 * Digimon World 2 card, in blocks 1 and on; `blocks` gets its length. */
static bool card_with_chain(const corpus_table *manifest, uint8_t *out, uint32_t *blocks) {
    uint8_t *image = (uint8_t *)malloc(PS1_CARD_SIZE);
    sigil_card_listing *l = NULL;
    bool ok = image && load_sample(manifest, "digimon-world-2-mcr", image) &&
              sigil_ps1_card_list(image, SIGIL_CARD_FORMAT_PS1_RAW, &l) == SIGIL_OK;
    const sigil_card_entry *e = NULL;
    for (size_t i = 0; ok && i < l->entry_count && !e; i++) {
        if (l->entries[i].blocks >= 2) e = &l->entries[i];
    }
    ok = ok && e;
    uint8_t *mcs = ok ? (uint8_t *)malloc(sigil_ps1_mcs_size(e->blocks)) : NULL;
    ok = ok && mcs && sigil_ps1_extract(image, e->first_block, e->blocks, mcs) == SIGIL_OK;
    if (ok) {
        sigil_ps1_format(out);
        *blocks = e->blocks;
        ok = sigil_ps1_inject(out, mcs, sigil_ps1_mcs_size(e->blocks)) == SIGIL_OK;
    }
    free(mcs);
    sigil_card_listing_free(l);
    free(image);
    return ok;
}

/* A chain whose link lands on a free block, on another save's first block,
 * or past the card is broken: the listing counts it corrupt and reads
 * nothing past the card. */
static void check_broken_chains(const corpus_table *manifest) {
    uint8_t *card = (uint8_t *)malloc(PS1_CARD_SIZE);
    uint8_t *broken = (uint8_t *)malloc(PS1_CARD_SIZE);
    uint32_t blocks = 0;
    if (!card || !broken || !card_with_chain(manifest, card, &blocks) || blocks + 1 > PS1_DATA_BLOCKS) {
        fail("broken chains", "setup failed");
        free(card);
        free(broken);
        return;
    }
    struct { const char *what; uint16_t link; bool foreign_first; } CASES[] = {
        { "a link to a free block", (uint16_t)blocks, false },
        { "a link to another save's first block", (uint16_t)blocks, true },
        { "a link past the card", 0x4942, false },
    };
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        memcpy(broken, card, PS1_CARD_SIZE);
        if (CASES[i].foreign_first) {
            uint8_t *f = broken + (size_t)(blocks + 1) * PS1_FRAME_SIZE;
            memcpy(f, broken + PS1_FRAME_SIZE, PS1_FRAME_SIZE);
            f[0x0A + 2] ^= 0x01;
            set_link(broken, blocks + 1, 0xFFFF);
        }
        set_link(broken, 1, CASES[i].link);
        sigil_card_listing *l = NULL;
        char name[PS1_NAME_LEN + 1] = { 0 };
        memcpy(name, card + PS1_FRAME_SIZE + 0x0A, PS1_NAME_LEN);
        if (sigil_ps1_card_list(broken, SIGIL_CARD_FORMAT_PS1_RAW, &l) != SIGIL_OK || l->corrupt_count < 1 ||
            find_entry(l, name)) {
            fail("broken chains", CASES[i].what);
        } else if (l->corrupt_entry_count < 1 || strcmp(l->corrupt_entries[0].name, name) != 0 ||
                   l->corrupt_entries[0].first_block != 1 || !l->corrupt_entries[0].owner_id[0]) {
            fail("broken chains", "the broken save isn't named among the corrupt entries");
        }
        sigil_card_listing_free(l);
    }

    /* Another save's chain running into this one's middle: each claims a
     * block of the other, so neither is whole and both are corrupt. */
    memcpy(broken, card, PS1_CARD_SIZE);
    uint8_t *f = broken + (size_t)(blocks + 1) * PS1_FRAME_SIZE;
    memcpy(f, broken + PS1_FRAME_SIZE, PS1_FRAME_SIZE);
    f[0x0A + 2] ^= 0x01;
    set_link(broken, blocks + 1, 1);
    sigil_card_listing *l = NULL;
    if (sigil_ps1_card_list(broken, SIGIL_CARD_FORMAT_PS1_RAW, &l) != SIGIL_OK || l->entry_count != 0 ||
        l->corrupt_count != 2 || l->corrupt_entry_count != 2) {
        fail("broken chains", "two saves sharing a block listed");
    }
    sigil_card_listing_free(l);
    free(broken);
    free(card);
}

/* sigil reads a raw card only at its own size, and injects only a .mcs whose
 * frame marks a save's first block. */
static void check_refusals(const corpus_table *manifest) {
    uint8_t *image = (uint8_t *)malloc(PS1_CARD_SIZE + 1);
    if (!image || !load_sample(manifest, "xenogears-full-mcd", image)) { free(image); fail("refusals", "setup failed"); return; }
    image[PS1_CARD_SIZE] = 0;
    uint8_t *loaded = (uint8_t *)malloc(PS1_CARD_SIZE);
    sigil_io *io = mem_root_io(image, PS1_CARD_SIZE + 1);
    int format = 0;
    if (sigil_ps1_card_load(io, loaded, &format) != SIGIL_ERR_UNSUPPORTED_FORMAT) fail("refusals", "a raw card longer than 128 KiB loaded");
    sigil_io_close(io);

    /* A raw card or .vmp cut short has lost saves; only a .gme may end early. */
    io = mem_root_io(image, 16 * 1024);
    if (sigil_ps1_card_load(io, loaded, &format) != SIGIL_ERR_UNSUPPORTED_FORMAT) fail("refusals", "a raw card cut short loaded");
    sigil_io_close(io);
    uint8_t *vmp = (uint8_t *)calloc(1, PS1_VMP_HEADER_SIZE + 16 * 1024);
    if (vmp) {
        memcpy(vmp, "\0PMV", 4);
        memcpy(vmp + PS1_VMP_HEADER_SIZE, image, 16 * 1024);
        io = mem_root_io(vmp, PS1_VMP_HEADER_SIZE + 16 * 1024);
        if (sigil_ps1_card_load(io, loaded, &format) != SIGIL_ERR_UNSUPPORTED_FORMAT) fail("refusals", "a .vmp cut short loaded");
        sigil_io_close(io);
        free(vmp);
    }

    sigil_card_listing *l = NULL;
    if (sigil_ps1_card_list(image, SIGIL_CARD_FORMAT_PS1_RAW, &l) == SIGIL_OK && l->entry_count > 0) {
        size_t len = sigil_ps1_mcs_size(l->entries[0].blocks);
        uint8_t *mcs = (uint8_t *)malloc(len);
        uint8_t *fresh = (uint8_t *)malloc(PS1_CARD_SIZE);
        if (mcs && fresh && sigil_ps1_extract(image, l->entries[0].first_block, l->entries[0].blocks, mcs) == SIGIL_OK) {
            mcs[0] = 0xA1;
            sigil_ps1_format(fresh);
            if (sigil_ps1_inject(fresh, mcs, len) != SIGIL_ERR_UNSUPPORTED_FORMAT) fail("refusals", "a .mcs that isn't a save's first block went in");
        }
        free(fresh);
        free(mcs);
    } else {
        fail("refusals", "setup failed");
    }
    sigil_card_listing_free(l);
    free(loaded);
    free(image);
}

#define VMP_FILE_SIZE  (PS1_VMP_HEADER_SIZE + PS1_CARD_SIZE)
#define VMP_SIGNATURE  0x20u
#define GME_FILE_SIZE  (PS1_GME_HEADER_SIZE + PS1_CARD_SIZE)
#define GME_STATES     0x16u
#define GME_LINKS      0x26u
#define GME_COMMENTS   0x40u
#define GME_COMMENT    0x100u

static sigil_ps1_file *file_from(const uint8_t *data, size_t len, int *rc) {
    sigil_ps1_file *f = (sigil_ps1_file *)malloc(sizeof(*f));
    sigil_io *io = mem_root_io(data, len);
    *rc = f ? sigil_ps1_file_load(io, f) : SIGIL_ERR_OOM;
    sigil_io_close(io);
    if (*rc != SIGIL_OK) { free(f); return NULL; }
    return f;
}

/* `f` written out and read back: the card must come back as it was. */
static uint8_t *written(const char *id, const sigil_ps1_file *f, size_t *len) {
    uint8_t *out = NULL;
    int rc = 0;
    if (sigil_ps1_file_write(f, &out, len) != SIGIL_OK) { fail(id, "write failed"); return NULL; }
    sigil_ps1_file *back = file_from(out, *len, &rc);
    if (!back || back->format != f->format || memcmp(back->image, f->image, PS1_CARD_SIZE) != 0) {
        fail(id, "the written file doesn't read back as the same card");
    }
    free(back);
    return out;
}

/* Swaps the card's first save for the Bugs Bunny save, as a restore would. */
static bool swap_first_save(const corpus_table *manifest, sigil_ps1_file *f, char removed[SIGIL_CARD_NAME_MAX],
                            char kept[SIGIL_CARD_NAME_MAX]) {
    size_t len = 0;
    uint8_t *mcs = load_sample_bytes(manifest, "bugs-bunny-mcs", &len);
    sigil_card_listing *l = NULL;
    bool ok = mcs && sigil_ps1_card_list(f->image, f->format, &l) == SIGIL_OK && l->entry_count >= 2;
    if (ok) {
        snprintf(removed, SIGIL_CARD_NAME_MAX, "%s", l->entries[0].name);
        snprintf(kept, SIGIL_CARD_NAME_MAX, "%s", l->entries[1].name);
        ok = sigil_ps1_delete(f->image, l->entries[0].first_block) == SIGIL_OK &&
             sigil_ps1_inject(f->image, mcs, len) == SIGIL_OK;
    }
    sigil_card_listing_free(l);
    free(mcs);
    return ok;
}

static uint32_t slot_of(const uint8_t *image, const char *name) {
    sigil_card_listing *l = NULL;
    uint32_t slot = UINT32_MAX;
    if (sigil_ps1_card_list(image, SIGIL_CARD_FORMAT_PS1_RAW, &l) != SIGIL_OK) return slot;
    const sigil_card_entry *e = find_entry(l, name);
    if (e) slot = e->first_block - 1;
    sigil_card_listing_free(l);
    return slot;
}

/* A PSP or Vita .vmp: an unchanged card writes back byte for byte, signature
 * and all; a changed one keeps the header and its seed, stays 0x20080 bytes
 * and carries a signature that checks. */
static void check_vmp(const corpus_table *manifest) {
    static const char *const IDS[] = { "vagrant-story-vmp", "suikoden2-vmp" };
    for (size_t i = 0; i < 2; i++) {
        size_t len = 0, out_len = 0;
        uint8_t *bytes = load_sample_bytes(manifest, IDS[i], &len);
        if (!bytes) continue;
        int rc = 0;
        sigil_ps1_file *f = file_from(bytes, len, &rc);
        uint8_t *out = NULL;
        if (!f || f->format != SIGIL_CARD_FORMAT_PS1_VMP || sigil_ps1_file_check(f) != SIGIL_OK) {
            fail(IDS[i], "a .vmp as the console signed it doesn't check");
        } else if (!(out = written(IDS[i], f, &out_len)) || out_len != len || memcmp(out, bytes, len) != 0) {
            fail(IDS[i], "an unchanged .vmp doesn't write back byte for byte");
        }
        free(out);
        out = NULL;

        char removed[SIGIL_CARD_NAME_MAX], kept[SIGIL_CARD_NAME_MAX];
        sigil_ps1_file *changed = NULL;
        if (f && i == 0) {
            sigil_ps1_format(f->image);
            size_t mcs_len = 0;
            uint8_t *mcs = load_sample_bytes(manifest, "bugs-bunny-mcs", &mcs_len);
            if (!mcs || sigil_ps1_inject(f->image, mcs, mcs_len) != SIGIL_OK) fail(IDS[i], "setup failed");
            free(mcs);
        } else if (f && !swap_first_save(manifest, f, removed, kept)) {
            fail(IDS[i], "setup failed");
        }
        if (f && (out = written(IDS[i], f, &out_len))) {
            if (out_len != VMP_FILE_SIZE || memcmp(out, bytes, VMP_SIGNATURE) != 0 ||
                memcmp(out + VMP_SIGNATURE + 20, bytes + VMP_SIGNATURE + 20, PS1_VMP_HEADER_SIZE - VMP_SIGNATURE - 20) != 0) {
                fail(IDS[i], "a rewritten .vmp lost its header or seed, or its size");
            }
            if (memcmp(out + VMP_SIGNATURE, bytes + VMP_SIGNATURE, 20) == 0) fail(IDS[i], "a changed card kept the old signature");
            changed = file_from(out, out_len, &rc);
            if (!changed || sigil_ps1_file_check(changed) != SIGIL_OK) fail(IDS[i], "a rewritten .vmp's signature doesn't check");
        }
        free(changed);
        free(out);
        out = NULL;
        free(f);

        /* One flipped byte of the card: damaged; written again, signed anew. */
        bytes[PS1_VMP_HEADER_SIZE + PS1_BLOCK_SIZE + 0x100] ^= 0xFF;
        sigil_ps1_file *bad = file_from(bytes, len, &rc), *fixed = NULL;
        if (!bad || sigil_ps1_file_check(bad) != SIGIL_ERR_DAMAGED) {
            fail(IDS[i], "a .vmp whose card doesn't match its signature didn't read as damaged");
        } else if ((out = written(IDS[i], bad, &out_len))) {
            fixed = file_from(out, out_len, &rc);
            if (!fixed || sigil_ps1_file_check(fixed) != SIGIL_OK || memcmp(fixed->image, bad->image, PS1_CARD_SIZE) != 0) {
                fail(IDS[i], "a damaged .vmp written again isn't signed for its card");
            }
        }
        free(fixed);
        free(out);
        free(bad);
        free(bytes);
    }
}

/* A DexDrive .gme: unchanged, it writes back byte for byte; changed, every
 * header byte but the frame copies and comments stays, the copies follow the
 * directory as DexDrive writes them, and a comment stays only beside its own
 * save. A short file writes back full size. */
static void check_gme(const corpus_table *manifest) {
    size_t len = 0, out_len = 0;
    uint8_t *bytes = load_sample_bytes(manifest, "gran-turismo-gme", &len);
    if (bytes) {
        int rc = 0;
        sigil_ps1_file *f = file_from(bytes, len, &rc);
        uint8_t *out = f ? written("gran-turismo-gme", f, &out_len) : NULL;
        if (!f || f->format != SIGIL_CARD_FORMAT_PS1_GME || !out || out_len != len || memcmp(out, bytes, len) != 0) {
            fail("gran-turismo-gme", "an unchanged .gme doesn't write back byte for byte");
        }
        free(out);
        out = NULL;
        free(f);
        f = NULL;

        char removed[SIGIL_CARD_NAME_MAX], kept[SIGIL_CARD_NAME_MAX];
        uint8_t *image = bytes + PS1_GME_HEADER_SIZE;
        uint32_t removed_slot = UINT32_MAX, kept_slot = UINT32_MAX, free_slot = UINT32_MAX;
        sigil_card_listing *l = NULL;
        if (sigil_ps1_card_list(image, SIGIL_CARD_FORMAT_PS1_RAW, &l) == SIGIL_OK && l->entry_count >= 2) {
            removed_slot = l->entries[0].first_block - 1;
            kept_slot = l->entries[1].first_block - 1;
        }
        sigil_card_listing_free(l);
        for (uint32_t s = 0; s < PS1_DATA_BLOCKS && free_slot == UINT32_MAX; s++) {
            uint8_t state = image[(s + 1) * PS1_FRAME_SIZE];
            if (state >= 0xA0 && state <= 0xA3) free_slot = s;
        }
        if (removed_slot != UINT32_MAX && free_slot != UINT32_MAX) {
            memcpy(bytes + GME_COMMENTS + GME_COMMENT * removed_slot, "the removed save", 17);
            memcpy(bytes + GME_COMMENTS + GME_COMMENT * kept_slot, "the kept save", 14);
            memcpy(bytes + GME_COMMENTS + GME_COMMENT * free_slot, "a deleted save", 15);
            f = file_from(bytes, len, &rc);
        }
        if (!f || removed_slot == UINT32_MAX || !swap_first_save(manifest, f, removed, kept) ||
            !(out = written("gran-turismo-gme", f, &out_len))) {
            fail("gran-turismo-gme", "setup failed");
        } else {
            uint8_t *card = out + PS1_GME_HEADER_SIZE;
            bool others = out_len == GME_FILE_SIZE && memcmp(out, bytes, GME_STATES) == 0 &&
                          out[GME_STATES + 15] == bytes[GME_STATES + 15] &&
                          memcmp(out + GME_LINKS + 15, bytes + GME_LINKS + 15, GME_COMMENTS - GME_LINKS - 15) == 0;
            if (!others) fail("gran-turismo-gme", "a header byte outside the frame copies and comments changed");
            for (uint32_t s = 0; s < PS1_DATA_BLOCKS; s++) {
                const uint8_t *frame = card + (s + 1) * PS1_FRAME_SIZE;
                bool deleted = frame[0] >= 0xA1 && frame[0] <= 0xA3;
                if (out[GME_STATES + s] != (deleted ? 0xA0 : frame[0]) || out[GME_LINKS + s] != (deleted ? 0xFF : frame[8])) {
                    fail("gran-turismo-gme", "a frame copy doesn't follow the directory");
                }
            }
            size_t mcs_len = 0;
            uint8_t *mcs = load_sample_bytes(manifest, "bugs-bunny-mcs", &mcs_len);
            char new_name[PS1_NAME_LEN + 1] = { 0 };
            if (mcs) memcpy(new_name, mcs + 0x0A, PS1_NAME_LEN);
            free(mcs);
            uint32_t now_kept = slot_of(card, kept);
            uint32_t new_slot = slot_of(card, new_name);
            if (now_kept != kept_slot || strcmp((const char *)out + GME_COMMENTS + GME_COMMENT * kept_slot, "the kept save") != 0) {
                fail("gran-turismo-gme", "the comment beside a save that stayed was lost");
            }
            const uint32_t cleared[2] = { removed_slot, new_slot };
            for (size_t c = 0; c < 2; c++) {
                const uint8_t *comment = out + GME_COMMENTS + GME_COMMENT * cleared[c];
                for (uint32_t b = 0; b < GME_COMMENT; b++) {
                    if (comment[b]) { fail("gran-turismo-gme", "a comment stayed on a slot whose save changed"); break; }
                }
            }
            if (new_slot != free_slot) fail("gran-turismo-gme", "setup: the new save didn't take the lowest free slot");
        }
        free(out);
        out = NULL;
        free(f);

        /* Another save in the same slot, same state byte: only its name says
         * the comment no longer belongs. */
        f = removed_slot != UINT32_MAX ? file_from(bytes, len, &rc) : NULL;
        if (f) {
            memcpy(f->image + (removed_slot + 1) * PS1_FRAME_SIZE + 0x0A, "BASLUS-00000OTHERSAV", PS1_NAME_LEN);
            out = written("gran-turismo-gme", f, &out_len);
            const uint8_t *comment = out ? out + GME_COMMENTS + GME_COMMENT * removed_slot : NULL;
            if (!comment || comment[0]) fail("gran-turismo-gme", "a comment stayed beside another save in its slot");
        }
        free(out);
        free(f);
        free(bytes);
    }

    bytes = load_sample_bytes(manifest, "sotn-short-gme", &len);
    if (bytes) {
        int rc = 0;
        sigil_ps1_file *f = file_from(bytes, len, &rc);
        uint8_t *out = f ? written("sotn-short-gme", f, &out_len) : NULL;
        bool zero_tail = out && out_len == GME_FILE_SIZE;
        for (size_t b = len; zero_tail && b < out_len; b++) zero_tail = out[b] == 0;
        if (!out || out_len != GME_FILE_SIZE || memcmp(out, bytes, len) != 0 || !zero_tail) {
            fail("sotn-short-gme", "a short .gme doesn't write back as the full card it reads as");
        }
        free(out);
        free(f);
        free(bytes);
    }
}

int main(void) {
    char path[1024];
    corpus_table manifest, entries;
    if (corpus_platform_path("psx", "manifest.tsv", path, sizeof(path)) != 0 ||
        corpus_load(path, &manifest) != 0) {
        fprintf(stderr, "SKIP: no psx manifest\n");
        return TEST_SKIP;
    }
    if (corpus_platform_path("psx", "entries.tsv", path, sizeof(path)) != 0 ||
        corpus_load(path, &entries) != 0) {
        corpus_free(&manifest);
        fprintf(stderr, "SKIP: no psx entries\n");
        return TEST_SKIP;
    }

    for (size_t r = 0; r < manifest.nrows; r++) {
        const char *kind = corpus_get(&manifest, r, "kind");
        const char *id = corpus_get(&manifest, r, "id");
        const char *file = corpus_get(&manifest, r, "path");
        if (!kind || !id || !file) continue;
        if (strcmp(kind, "card") == 0) check_card(id, file, &entries);
        else if (strcmp(kind, "wrapped") == 0) check_not_card(id, file);
    }
    check_format_matches_emulator(&manifest);
    check_mcs_round_trip(&manifest);
    check_inject_keeps_other_saves(&manifest);
    check_full_card_refuses(&manifest);
    check_broken_chains(&manifest);
    check_refusals(&manifest);
    check_vmp(&manifest);
    check_gme(&manifest);
    int missing = g_cards ? corpus_count_missing(&manifest, "psx") : 0;
    corpus_free(&manifest);
    corpus_free(&entries);

    printf("ps1 cards: %d checked, %d failures\n", g_cards, g_fails);
    if (!g_cards) return TEST_SKIP;
    return corpus_exit(g_fails + missing);
}
