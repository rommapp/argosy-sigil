// SPDX-License-Identifier: MPL-2.0
#include "save_corpus.h"
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
        if (sigil_card_list(io, &public_listing) != SIGIL_OK ||
            public_listing->entry_count != listing->entry_count) {
            fail(id, "sigil_card_list disagrees with the internal listing");
        }
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
        if (sigil_ps1_inject(image, mcs, mcs_len) != SIGIL_ERR_NOT_FOUND) fail("xenogears-full-mcd", "full card took a save");
        if (memcmp(copy, image, PS1_CARD_SIZE) != 0) fail("xenogears-full-mcd", "a refused inject changed the card");
    }
    free(copy);
    free(image);
    free(mcs);
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
    corpus_free(&manifest);
    corpus_free(&entries);

    printf("ps1 cards: %d checked, %d failures\n", g_cards, g_fails);
    if (g_fails) return 1;
    return g_cards ? 0 : TEST_SKIP;
}
