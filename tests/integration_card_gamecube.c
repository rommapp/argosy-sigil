// SPDX-License-Identifier: MPL-2.0
#include "save_corpus.h"
#include "card_gamecube.h"
#include <stdbool.h>

#define TEST_SKIP 77
#define CARD_2MIB (256u * GC_BLOCK_SIZE)
#define CARD_512KIB (64u * GC_BLOCK_SIZE)

static int g_fails = 0;
static int g_checks = 0;

static void fail(const char *id, const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", id, what);
    g_fails++;
}

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

/* entries.tsv stores the game code and maker code as text ("GFZE8P"); the
 * listing reports the four game code bytes as uppercase hex ("47465A45"). */
static void expected_owner(const char *raw, char out[SIGIL_CARD_OWNER_MAX]) {
    out[0] = '\0';
    if (strcmp(raw, "-") == 0 || strlen(raw) < 4) return;
    snprintf(out, SIGIL_CARD_OWNER_MAX, "%02X%02X%02X%02X",
             (unsigned char)raw[0], (unsigned char)raw[1], (unsigned char)raw[2], (unsigned char)raw[3]);
}

static void md5_hex(const uint8_t *data, size_t len, char out[33]) {
    sigil_md5 m;
    uint8_t digest[16];
    sigil_md5_init(&m);
    sigil_md5_update(&m, data, len);
    sigil_md5_final(&m, digest);
    sigil_md5_hex(digest, out);
}

static void entry_md5(const uint8_t *image, size_t size, const sigil_card_entry *e, char out[33]) {
    out[0] = '\0';
    size_t len = (size_t)e->blocks * GC_BLOCK_SIZE;
    uint8_t *data = (uint8_t *)malloc(len);
    if (!data) return;
    if (sigil_gamecube_entry_data(image, size, e->first_block, e->blocks, data) == SIGIL_OK) md5_hex(data, len, out);
    free(data);
}

static const sigil_card_entry *find_entry(const sigil_card_listing *l, const char *name) {
    for (size_t i = 0; i < l->entry_count; i++) {
        if (strcmp(l->entries[i].name, name) == 0) return &l->entries[i];
    }
    return NULL;
}

/* Checks that `listing` holds exactly the entries.tsv rows for id/path, less
 * the one named `except` when given. */
static void check_expected_entries(const char *where, const char *id, const char *path,
                                   const corpus_table *entries, const sigil_card_listing *listing,
                                   const uint8_t *image, size_t size, const char *except) {
    size_t expected = 0;
    for (size_t r = 0; r < entries->nrows; r++) {
        if (strcmp(corpus_get(entries, r, "id"), id) != 0) continue;
        if (strcmp(corpus_get(entries, r, "path"), path) != 0) continue;
        const char *name = corpus_get(entries, r, "entry");
        char label[256];
        snprintf(label, sizeof(label), "%s %s", where, name);
        const sigil_card_entry *e = find_entry(listing, name);
        if (except && strcmp(name, except) == 0) {
            if (e) fail(label, "entry listed though its chain is broken");
            continue;
        }
        expected++;
        if (!e) { fail(label, "entry not listed"); continue; }

        char owner[SIGIL_CARD_OWNER_MAX];
        expected_owner(corpus_get(entries, r, "owner_id"), owner);
        if (strcmp(owner, e->owner_id) != 0) fail(label, "owner_id differs");
        if (strtoul(corpus_get(entries, r, "blocks"), NULL, 10) != e->blocks) fail(label, "block count differs");

        char md5[33];
        entry_md5(image, size, e, md5);
        if (strcmp(md5, corpus_get(entries, r, "data_md5")) != 0) fail(label, "data md5 differs");
    }
    if (expected != listing->entry_count) fail(where, "listing holds entries entries.tsv doesn't");
}

typedef struct {
    const char *id;
    uint32_t    total_blocks;
    uint32_t    free_blocks;
    uint32_t    free_slots;
    uint32_t    corrupt_count;
} card_fact;

/* Free blocks from each card's current allocation table by hex dump (USA:
 * BAT block 4, 0x8006 = 0x00BF; Nintendont: BAT block 4, 0x8006 = 0x00F9).
 * Free slots are 127 less the entries.tsv rows. */
static const card_fact CARD_FACTS[] = {
    { "card-raw-usa",            251, 191, 117, 0 },
    { "card-raw-jpn-nintendont", 251, 249, 126, 0 },
};

static void check_facts(const char *id, const sigil_card_listing *listing) {
    bool known = false;
    for (size_t i = 0; i < sizeof(CARD_FACTS) / sizeof(CARD_FACTS[0]); i++) {
        if (strcmp(CARD_FACTS[i].id, id) != 0) continue;
        known = true;
        if (listing->format != SIGIL_CARD_FORMAT_GAMECUBE_RAW) fail(id, "format differs");
        if (listing->total_blocks != CARD_FACTS[i].total_blocks) fail(id, "total block count differs");
        if (listing->free_blocks != CARD_FACTS[i].free_blocks) fail(id, "free block count differs");
        if (listing->free_slots != CARD_FACTS[i].free_slots) fail(id, "free slot count differs");
        if (listing->corrupt_count != CARD_FACTS[i].corrupt_count) fail(id, "corrupt count differs");
    }
    if (!known) fail(id, "card sample has no facts row");
}

typedef struct {
    uint8_t *gci[GC_DIR_ENTRIES];
    size_t   len[GC_DIR_ENTRIES];
    size_t   count;
} save_set;

static void save_set_free(save_set *s) {
    for (size_t i = 0; i < s->count; i++) free(s->gci[i]);
    s->count = 0;
}

static bool extract_all(const uint8_t *image, size_t size, const sigil_card_listing *l, save_set *out) {
    out->count = 0;
    for (size_t i = 0; i < l->entry_count; i++) {
        size_t len = sigil_gamecube_gci_size(l->entries[i].blocks);
        uint8_t *gci = (uint8_t *)malloc(len);
        if (!gci || sigil_gamecube_extract(image, size, l->entries[i].first_block, l->entries[i].blocks, gci) != SIGIL_OK) {
            free(gci);
            save_set_free(out);
            return false;
        }
        out->gci[out->count] = gci;
        out->len[out->count++] = len;
    }
    return true;
}

static bool build_card(const save_set *saves, uint8_t *image, size_t size, bool shift_jis) {
    if (sigil_gamecube_format(image, size, shift_jis) != SIGIL_OK) return false;
    for (size_t i = 0; i < saves->count; i++) {
        if (sigil_gamecube_inject(image, size, saves->gci[i], saves->len[i]) != SIGIL_OK) return false;
    }
    return true;
}

static bool card_is_shift_jis(const uint8_t *image) {
    return be16(image + 0x24) == 1;
}

/* Rebuilding a card from its own saves keeps every save's bytes, builds the
 * same card twice, and leaves each save verifiable. */
static void check_rebuild(const char *id, const char *path, const corpus_table *entries,
                          const uint8_t *image, size_t size, const sigil_card_listing *listing) {
    save_set saves;
    if (!extract_all(image, size, listing, &saves)) { fail(id, "extract failed"); return; }
    for (size_t i = 0; i < saves.count; i++) {
        if (be16(saves.gci[i] + 0x36) != listing->entries[i].first_block) {
            fail(id, "an extracted .gci names another first block than the card");
        }
    }

    uint8_t *built = (uint8_t *)malloc(size);
    uint8_t *again = (uint8_t *)malloc(size);
    sigil_card_listing *rebuilt = NULL;
    bool sjis = card_is_shift_jis(image);
    if (!built || !again || !build_card(&saves, built, size, sjis) || !build_card(&saves, again, size, sjis)) {
        fail(id, "rebuild failed");
    } else if (memcmp(built, again, size) != 0) {
        fail(id, "two builds from the same saves differ");
    } else if (sigil_gamecube_card_list(built, size, &rebuilt) != SIGIL_OK) {
        fail(id, "rebuilt card did not list");
    } else {
        char label[256];
        snprintf(label, sizeof(label), "%s (rebuilt)", id);
        check_expected_entries(label, id, path, entries, rebuilt, built, size, NULL);
        for (size_t i = 0; i < saves.count; i++) {
            if (sigil_gamecube_verify(built, size, saves.gci[i], saves.len[i]) != SIGIL_OK) fail(label, "a save did not verify");
        }
    }
    sigil_card_listing_free(rebuilt);
    free(built);
    free(again);
    save_set_free(&saves);
}

static bool load_card_file(const char *full, uint8_t **image, size_t *size) {
    sigil_io *io = sigil_io_open_file(full);
    if (!io) return false;
    int rc = sigil_gamecube_card_load(io, image, size);
    sigil_io_close(io);
    return rc == SIGIL_OK;
}

static void check_card(const char *id, const char *path, const corpus_table *entries) {
    char full[1024];
    if (corpus_sample_path("ngc", id, path, full, sizeof(full)) != 0) return;
    sigil_io *io = sigil_io_open_file(full);
    if (!io) return;
    g_checks++;

    uint8_t *image = NULL;
    size_t size = 0;
    sigil_card_listing *listing = NULL;
    if (sigil_gamecube_card_load(io, &image, &size) != SIGIL_OK ||
        sigil_gamecube_card_list(image, size, &listing) != SIGIL_OK) {
        fail(id, "card did not load");
    } else {
        check_expected_entries(id, id, path, entries, listing, image, size, NULL);
        check_facts(id, listing);
        check_rebuild(id, path, entries, image, size, listing);
        sigil_card_listing *public_listing = NULL;
        if (sigil_card_list(io, &public_listing) != SIGIL_OK ||
            public_listing->format != SIGIL_CARD_FORMAT_GAMECUBE_RAW ||
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
    if (corpus_sample_path("ngc", id, path, full, sizeof(full)) != 0) return;
    sigil_io *io = sigil_io_open_file(full);
    if (!io) return;
    g_checks++;
    sigil_card_listing *listing = NULL;
    if (sigil_card_list(io, &listing) != SIGIL_ERR_UNSUPPORTED_FORMAT) fail(id, "a single save read as a card");
    sigil_card_listing_free(listing);
    listing = NULL;
    if (sigil_gamecube_card_list_io(io, &listing) != SIGIL_ERR_UNSUPPORTED_FORMAT) fail(id, "the GameCube reader took a single save");
    sigil_card_listing_free(listing);
    sigil_io_close(io);
}

static uint8_t *load_sample_bytes(const corpus_table *manifest, const char *id, const char *path, size_t *len) {
    for (size_t r = 0; r < manifest->nrows; r++) {
        if (strcmp(corpus_get(manifest, r, "id"), id) != 0) continue;
        if (path && strcmp(corpus_get(manifest, r, "path"), path) != 0) continue;
        char full[1024];
        if (corpus_sample_path("ngc", id, corpus_get(manifest, r, "path"), full, sizeof(full)) != 0) return NULL;
        return corpus_read_file(full, len);
    }
    return NULL;
}

static bool load_card_sample(const corpus_table *manifest, const char *id, uint8_t **image, size_t *size) {
    for (size_t r = 0; r < manifest->nrows; r++) {
        if (strcmp(corpus_get(manifest, r, "id"), id) != 0) continue;
        char full[1024];
        if (corpus_sample_path("ngc", id, corpus_get(manifest, r, "path"), full, sizeof(full)) != 0) return false;
        return load_card_file(full, image, size);
    }
    return false;
}

static uint8_t *sample_as_gci(const corpus_table *manifest, const char *id, const char *path, size_t *gci_len) {
    size_t len = 0;
    uint8_t *raw = load_sample_bytes(manifest, id, path, &len);
    if (!raw) return NULL;
    uint8_t *gci = (uint8_t *)malloc(len);
    if (!gci || sigil_gamecube_to_gci(raw, len, gci, gci_len) != SIGIL_OK) {
        free(gci);
        gci = NULL;
    }
    free(raw);
    return gci;
}

/* Every single-save sample converts to a .gci that goes onto an empty card,
 * lists as entries.tsv says, and comes back byte for byte. The first-block
 * field is the one exception: it names where the save sits on the card, so
 * it reads 5, the first data block, after the round trip. */
static void check_save_sample(const char *id, const char *path, const char *kind,
                              const corpus_table *manifest, const corpus_table *entries) {
    size_t raw_len = 0;
    uint8_t *raw = load_sample_bytes(manifest, id, path, &raw_len);
    if (!raw) return;
    g_checks++;
    uint8_t *gci = (uint8_t *)malloc(raw_len);
    uint8_t *image = (uint8_t *)malloc(CARD_2MIB);
    size_t gci_len = 0;
    sigil_card_listing *l = NULL;
    if (!gci || !image || sigil_gamecube_to_gci(raw, raw_len, gci, &gci_len) != SIGIL_OK) {
        fail(id, "save did not convert to .gci");
    } else if (strcmp(kind, "gci") == 0 && (gci_len != raw_len || memcmp(gci, raw, raw_len) != 0)) {
        fail(id, "a .gci changed on conversion to .gci");
    } else if (sigil_gamecube_format(image, CARD_2MIB, false) != SIGIL_OK ||
               sigil_gamecube_inject(image, CARD_2MIB, gci, gci_len) != SIGIL_OK ||
               sigil_gamecube_card_list(image, CARD_2MIB, &l) != SIGIL_OK || l->entry_count != 1) {
        fail(id, "inject into an empty card failed");
    } else {
        check_expected_entries(id, id, path, entries, l, image, CARD_2MIB, NULL);
        if (memcmp(image + GC_BLOCK_SIZE, image + 2 * GC_BLOCK_SIZE, GC_BLOCK_SIZE) != 0 ||
            memcmp(image + 3 * GC_BLOCK_SIZE, image + 4 * GC_BLOCK_SIZE, GC_BLOCK_SIZE) != 0) {
            fail(id, "inject did not write both directory and allocation table copies");
        }
        uint8_t *back = (uint8_t *)malloc(gci_len);
        if (back && sigil_gamecube_extract(image, CARD_2MIB, l->entries[0].first_block, l->entries[0].blocks, back) == SIGIL_OK) {
            if (be16(back + 0x36) != 5) fail(id, "extracted first block is not the card's first data block");
            if (memcmp(back, gci, 0x36) != 0 || memcmp(back + 0x38, gci + 0x38, gci_len - 0x38) != 0) {
                fail(id, "the save changed on its way through a card");
            }
        } else {
            fail(id, "extract failed");
        }
        if (sigil_gamecube_verify(image, CARD_2MIB, gci, gci_len) != SIGIL_OK) fail(id, "injected save did not verify");
        free(back);
    }
    sigil_card_listing_free(l);
    free(image);
    free(gci);
    free(raw);
}

/* The GameShark and MaxDrive samples hold the same save (manifest notes).
 * Both convert to one .gci whose directory entry is the one the .gcs carries
 * at 0x110. */
static void check_wrappers_agree(const corpus_table *manifest) {
    size_t gcs_len = 0, sav_len = 0, raw_len = 0;
    uint8_t *from_gcs = sample_as_gci(manifest, "soulcalibur2-gcs", NULL, &gcs_len);
    uint8_t *from_sav = sample_as_gci(manifest, "soulcalibur2-maxdrive-sav", NULL, &sav_len);
    uint8_t *gcs = load_sample_bytes(manifest, "soulcalibur2-gcs", NULL, &raw_len);
    if (from_gcs && from_sav && gcs) {
        g_checks++;
        if (gcs_len != sav_len || memcmp(from_gcs, from_sav, gcs_len) != 0) fail("soulcalibur2", ".gcs and .sav convert to different saves");
        if (raw_len < 0x150 || memcmp(from_gcs, gcs + 0x110, 0x40) != 0) fail("soulcalibur2", "converted entry differs from the .gcs entry");

        /* GameShark can leave the stored block count at 1; the sample's
         * length, (33104 - 0x150) / 0x2000, says 4. */
        uint8_t *converted = (uint8_t *)malloc(raw_len);
        size_t converted_len = 0;
        gcs[0x110 + 0x38] = 0x00;
        gcs[0x110 + 0x39] = 0x01;
        if (!converted || sigil_gamecube_to_gci(gcs, raw_len, converted, &converted_len) != SIGIL_OK ||
            be16(converted + 0x38) != 4 || converted_len != gcs_len) {
            fail("soulcalibur2-gcs", "block count not taken from the .gcs length");
        }
        free(converted);
    }
    free(gcs);
    free(from_gcs);
    free(from_sav);
}

/* The Nintendont card still holds its formatted directory and allocation
 * table in blocks 1 and 3 (update counter 0, hex dump at 0x2000 and 0x6000);
 * a card sigil formats at the same size and encoding matches them. */
static void check_format_matches_console(const corpus_table *manifest) {
    size_t len = 0;
    uint8_t *sample = load_sample_bytes(manifest, "card-raw-jpn-nintendont", NULL, &len);
    uint8_t *image = (uint8_t *)malloc(CARD_2MIB);
    if (sample && image && len == CARD_2MIB) {
        g_checks++;
        if (sigil_gamecube_format(image, CARD_2MIB, true) != SIGIL_OK) {
            fail("format", "2 MiB format refused");
        } else {
            if (memcmp(image + GC_BLOCK_SIZE, sample + GC_BLOCK_SIZE, GC_BLOCK_SIZE) != 0) fail("format", "directory differs from Nintendont's");
            if (memcmp(image + 3 * GC_BLOCK_SIZE, sample + 3 * GC_BLOCK_SIZE, GC_BLOCK_SIZE) != 0) fail("format", "allocation table differs from Nintendont's");
            if (memcmp(image + 0x22, sample + 0x22, 4) != 0) fail("format", "size or encoding differs from Nintendont's");
        }
        if (sigil_gamecube_format(image, CARD_2MIB + GC_BLOCK_SIZE, false) != SIGIL_ERR_INVALID_ARG) fail("format", "an odd size formatted");
    }
    free(image);
    free(sample);
}

/* f_zero.dat.gci and f_zero.dat0.gci share one identity (manifest notes), so
 * the second is refused and the card is left as it was. Its banner flags
 * (0x07) are changed first: they are not part of the identity. */
static void check_same_identity_refused(const corpus_table *manifest) {
    size_t a_len = 0, b_len = 0;
    uint8_t *a = load_sample_bytes(manifest, "fzero-gx-dolphin-gci-set", "8P-GFZE-f_zero.dat.gci", &a_len);
    uint8_t *b = load_sample_bytes(manifest, "fzero-gx-dolphin-gci-set", "8P-GFZE-f_zero.dat0.gci", &b_len);
    uint8_t *image = (uint8_t *)malloc(CARD_2MIB);
    uint8_t *copy = (uint8_t *)malloc(CARD_2MIB);
    if (a && b && image && copy) {
        g_checks++;
        sigil_gamecube_format(image, CARD_2MIB, false);
        if (sigil_gamecube_inject(image, CARD_2MIB, a, a_len) != SIGIL_OK) {
            fail("fzero-gx-dolphin-gci-set", "first f_zero.dat refused");
        } else {
            memcpy(copy, image, CARD_2MIB);
            b[0x07] ^= 0x01;
            if (sigil_gamecube_inject(image, CARD_2MIB, b, b_len) != SIGIL_ERR_EXISTS) fail("fzero-gx-dolphin-gci-set", "a second save of the same identity went on");
            if (memcmp(copy, image, CARD_2MIB) != 0) fail("fzero-gx-dolphin-gci-set", "a refused inject changed the card");
            /* Dolphin compares file names up to the NUL: bytes after it don't make another save. */
            b[0x07] ^= 0x01;
            b[0x08 + 20] = 'X';
            if (sigil_gamecube_inject(image, CARD_2MIB, b, b_len) != SIGIL_ERR_EXISTS) {
                fail("fzero-gx-dolphin-gci-set", "bytes after a file name's NUL made a second save");
            }
        }
    }
    free(copy);
    free(image);
    free(b);
    free(a);
}

/* The USA card's saves take 7+8+4+15+4+7+3+5+4 = 57 blocks before
 * RogueLeader's 3 (entries.tsv, directory order); a 59-block card fits the
 * first nine and refuses the tenth without writing. */
static void check_no_room_for_blocks(const corpus_table *manifest) {
    uint8_t *usa = NULL;
    size_t usa_size = 0;
    if (!load_card_sample(manifest, "card-raw-usa", &usa, &usa_size)) return;
    g_checks++;
    sigil_card_listing *l = NULL;
    save_set saves = {0};
    uint8_t *image = (uint8_t *)malloc(CARD_512KIB);
    uint8_t *copy = (uint8_t *)malloc(CARD_512KIB);
    if (!image || !copy || sigil_gamecube_card_list(usa, usa_size, &l) != SIGIL_OK || l->entry_count != 10 ||
        !extract_all(usa, usa_size, l, &saves) || sigil_gamecube_format(image, CARD_512KIB, false) != SIGIL_OK) {
        fail("card-raw-usa", "could not set up the small card");
    } else {
        for (size_t i = 0; i < 9; i++) {
            if (sigil_gamecube_inject(image, CARD_512KIB, saves.gci[i], saves.len[i]) != SIGIL_OK) fail("card-raw-usa", "a save that fits was refused");
        }
        memcpy(copy, image, CARD_512KIB);
        if (strcmp(l->entries[9].name, "RogueLeader") != 0) fail("card-raw-usa", "directory order differs");
        if (sigil_gamecube_inject(image, CARD_512KIB, saves.gci[9], saves.len[9]) != SIGIL_ERR_NO_SPACE) fail("card-raw-usa", "a card without room took a save");
        if (memcmp(copy, image, CARD_512KIB) != 0) fail("card-raw-usa", "a refused inject changed the card");
    }
    save_set_free(&saves);
    sigil_card_listing_free(l);
    free(copy);
    free(image);
    free(usa);
}

/* A directory holds 127 entries; the 128th save is refused without writing. */
static void check_no_room_in_directory(const corpus_table *manifest) {
    size_t len = 0;
    uint8_t *gci = load_sample_bytes(manifest, "bleach-gc-jp-gci", NULL, &len);
    uint8_t *image = (uint8_t *)malloc(CARD_2MIB);
    uint8_t *copy = (uint8_t *)malloc(CARD_2MIB);
    if (gci && image && copy && len > 0x40) {
        g_checks++;
        sigil_gamecube_format(image, CARD_2MIB, true);
        for (int i = 0; i < 127; i++) {
            snprintf((char *)gci + 0x08 + 8, 8, "%03d", i);
            if (sigil_gamecube_inject(image, CARD_2MIB, gci, len) != SIGIL_OK) fail("bleach-gc-jp-gci", "a save with a free slot was refused");
        }
        snprintf((char *)gci + 0x08 + 8, 8, "full");
        memcpy(copy, image, CARD_2MIB);
        if (sigil_gamecube_inject(image, CARD_2MIB, gci, len) != SIGIL_ERR_NO_SPACE) fail("bleach-gc-jp-gci", "a full directory took a save");
        if (memcmp(copy, image, CARD_2MIB) != 0) fail("bleach-gc-jp-gci", "a refused inject changed the card");
    }
    free(copy);
    free(image);
    free(gci);
}

/* Deleting f_zero.dat (4 blocks) from the USA card frees its blocks and slot,
 * leaves every other save verifiable, and makes room for the same identity. */
static void check_delete(const corpus_table *manifest, const corpus_table *entries) {
    uint8_t *image = NULL;
    size_t size = 0;
    if (!load_card_sample(manifest, "card-raw-usa", &image, &size)) return;
    g_checks++;
    sigil_card_listing *before = NULL, *after = NULL;
    save_set kept = {0};
    if (sigil_gamecube_card_list(image, size, &before) != SIGIL_OK || !extract_all(image, size, before, &kept)) {
        fail("card-raw-usa", "could not list before delete");
    } else {
        const sigil_card_entry *fz = find_entry(before, "f_zero.dat");
        if (!fz || sigil_gamecube_delete(image, size, fz->first_block) != SIGIL_OK ||
            sigil_gamecube_card_list(image, size, &after) != SIGIL_OK) {
            fail("card-raw-usa", "delete failed");
        } else {
            if (find_entry(after, "f_zero.dat")) fail("card-raw-usa", "deleted save still listed");
            if (after->free_blocks != 191 + 4 || after->free_slots != 117 + 1) fail("card-raw-usa", "delete did not free the save's blocks and slot");
            const uint8_t *first = image + (size_t)fz->first_block * GC_BLOCK_SIZE;
            for (size_t i = 0; i < GC_BLOCK_SIZE; i++) {
                if (first[i] != 0xFF) { fail("card-raw-usa", "delete left the save's data behind"); break; }
            }
            check_expected_entries("card-raw-usa (deleted)", "card-raw-usa", "memcard-image.raw", entries,
                                   after, image, size, "f_zero.dat");
            for (size_t i = 0; i < kept.count; i++) {
                bool is_fzero = strcmp(before->entries[i].name, "f_zero.dat") == 0;
                int rc = sigil_gamecube_verify(image, size, kept.gci[i], kept.len[i]);
                if (is_fzero && rc != SIGIL_ERR_NOT_FOUND) fail("card-raw-usa", "deleted save still verifies");
                if (!is_fzero && rc != SIGIL_OK) fail("card-raw-usa", "delete touched another save");
            }
            for (size_t i = 0; i < kept.count; i++) {
                if (strcmp(before->entries[i].name, "f_zero.dat") != 0) continue;
                if (sigil_gamecube_inject(image, size, kept.gci[i], kept.len[i]) != SIGIL_OK) fail("card-raw-usa", "freed room was not reusable");
                if (sigil_gamecube_verify(image, size, kept.gci[i], kept.len[i]) != SIGIL_OK) fail("card-raw-usa", "re-injected save did not verify");
            }
        }
    }
    save_set_free(&kept);
    sigil_card_listing_free(before);
    sigil_card_listing_free(after);
    free(image);
}

static void reseal_bat_with_counter(uint8_t *image, uint32_t block, uint16_t counter) {
    uint8_t *bat = image + (size_t)block * GC_BLOCK_SIZE;
    bat[4] = (uint8_t)(counter >> 8);
    bat[5] = (uint8_t)counter;
    uint16_t sum, inverse;
    sigil_gamecube_checksum(bat + 4, GC_BLOCK_SIZE - 4, &sum, &inverse);
    bat[0] = (uint8_t)(sum >> 8);
    bat[1] = (uint8_t)sum;
    bat[2] = (uint8_t)(inverse >> 8);
    bat[3] = (uint8_t)inverse;
}

typedef enum { EXPECT_BAT4, EXPECT_BAT3 } bat_expectation;

/* The USA card's two allocation tables differ only in RogueLeader's chain,
 * which BAT block 3 (counter 40, 0x6006 free = 0x00C2 = 194) lacks and BAT
 * block 4 (counter 41) holds. Which one lists shows which copy was chosen. */
/* A second directory entry naming the first save's chain under another
 * name: deleting either would free the other's blocks, so both are corrupt. */
static void check_shared_chain(const corpus_table *manifest) {
    uint8_t *image = NULL;
    size_t size = 0;
    if (!load_card_sample(manifest, "card-raw-usa", &image, &size)) return;
    g_checks++;
    for (uint32_t block = 1; block <= 2; block++) {
        uint8_t *dir = image + (size_t)block * GC_BLOCK_SIZE;
        uint8_t *spare = NULL;
        for (uint32_t i = 0; i < 127 && !spare; i++) {
            if (dir[i * 64] == 0xFF) spare = dir + i * 64;
        }
        if (!spare) { fail("shared chain", "setup failed"); free(image); return; }
        memcpy(spare, dir, 64);
        spare[0x08] ^= 0x20;
        uint16_t sum, inverse;
        sigil_gamecube_checksum(dir, 0x1FFC, &sum, &inverse);
        dir[0x1FFC] = (uint8_t)(sum >> 8);
        dir[0x1FFD] = (uint8_t)sum;
        dir[0x1FFE] = (uint8_t)(inverse >> 8);
        dir[0x1FFF] = (uint8_t)inverse;
    }
    sigil_card_listing *l = NULL;
    if (sigil_gamecube_card_list(image, size, &l) != SIGIL_OK || l->corrupt_count != 2 || l->corrupt_entry_count != 2) {
        fail("shared chain", "two entries sharing a chain listed");
    }
    sigil_card_listing_free(l);
    free(image);
}

static void check_bat_choice(const char *what, const uint8_t *usa, size_t size,
                             const corpus_table *entries, uint8_t *image, bat_expectation expect) {
    sigil_card_listing *l = NULL;
    if (sigil_gamecube_card_list(image, size, &l) != SIGIL_OK) {
        fail(what, "card did not list");
    } else if (expect == EXPECT_BAT4) {
        check_expected_entries(what, "card-raw-usa", "memcard-image.raw", entries, l, image, size, NULL);
        if (l->free_blocks != 191) fail(what, "free blocks are not BAT block 4's");
    } else {
        check_expected_entries(what, "card-raw-usa", "memcard-image.raw", entries, l, image, size, "RogueLeader");
        if (l->corrupt_count != 1 || l->free_blocks != 194) fail(what, "listing is not BAT block 3's");
        if (l->corrupt_entry_count != 1 || strcmp(l->corrupt_entries[0].name, "RogueLeader") != 0 ||
            strlen(l->corrupt_entries[0].owner_id) != 8) {
            fail(what, "RogueLeader isn't named among the corrupt entries");
        }
    }
    sigil_card_listing_free(l);
    memcpy(image, usa, size);
}

static void check_copy_choice(const corpus_table *manifest, const corpus_table *entries) {
    uint8_t *usa = NULL;
    size_t size = 0;
    if (!load_card_sample(manifest, "card-raw-usa", &usa, &size)) return;
    g_checks++;
    uint8_t *image = (uint8_t *)malloc(size);
    if (!image) { free(usa); return; }
    memcpy(image, usa, size);

    /* Directory block 1 (counter 334) is current; damage the first byte of
     * super_mario_sunshine's name there and block 2 (counter 333, same
     * entries) must take over. */
    image[GC_BLOCK_SIZE + 0x08] ^= 0x20;
    sigil_card_listing *l = NULL;
    if (sigil_gamecube_card_list(image, size, &l) != SIGIL_OK) {
        fail("dir fallback", "card with one bad directory did not list");
    } else {
        check_expected_entries("dir fallback", "card-raw-usa", "memcard-image.raw", entries, l, image, size, NULL);
    }
    sigil_card_listing_free(l);
    image[2 * GC_BLOCK_SIZE + 0x08] ^= 0x20;
    if (sigil_gamecube_card_list(image, size, &l) != SIGIL_ERR_UNSUPPORTED_FORMAT) fail("dir fallback", "card with two bad directories listed");
    sigil_card_listing_free(l);
    memcpy(image, usa, size);

    check_bat_choice("bat newer second", usa, size, entries, image, EXPECT_BAT4);

    image[4 * GC_BLOCK_SIZE + GC_BLOCK_SIZE - 1] ^= 0x01;
    check_bat_choice("bat fallback", usa, size, entries, image, EXPECT_BAT3);

    reseal_bat_with_counter(image, 3, 42);
    check_bat_choice("bat newer first", usa, size, entries, image, EXPECT_BAT3);

    reseal_bat_with_counter(image, 3, 41);
    check_bat_choice("bat counters equal", usa, size, entries, image, EXPECT_BAT3);

    reseal_bat_with_counter(image, 3, 0x8000);
    check_bat_choice("bat counter signed", usa, size, entries, image, EXPECT_BAT4);

    image[0x10] ^= 0x01;
    l = NULL;
    if (sigil_gamecube_card_list(image, size, &l) != SIGIL_ERR_UNSUPPORTED_FORMAT) fail("header", "card with a bad header checksum listed");
    sigil_card_listing_free(l);

    free(image);
    free(usa);
}

/* F-Zero GX's f_zero.dat on the USA card carries that card's serial. Moved to
 * a sigil card and bound there, it changes; moved back to the USA card and
 * bound again, its data matches the entries.tsv md5 for the USA card. */
static void check_bind_serial(const corpus_table *manifest, const corpus_table *entries) {
    uint8_t *usa = NULL;
    size_t size = 0;
    if (!load_card_sample(manifest, "card-raw-usa", &usa, &size)) return;
    g_checks++;
    uint8_t *ours = (uint8_t *)malloc(CARD_2MIB);
    sigil_card_listing *l = NULL, *ours_l = NULL, *back_l = NULL;
    save_set saves = {0};
    uint8_t *moved = NULL;
    if (!ours || sigil_gamecube_card_list(usa, size, &l) != SIGIL_OK || !extract_all(usa, size, l, &saves)) {
        fail("bind", "setup failed");
        goto done;
    }
    const sigil_card_entry *fz = find_entry(l, "f_zero.dat");
    const sigil_card_entry *mario = find_entry(l, "super_mario_sunshine");
    size_t fz_index = fz ? (size_t)(fz - l->entries) : 0;
    moved = fz ? (uint8_t *)malloc(saves.len[fz_index]) : NULL;
    if (!fz || !mario || !moved) { fail("bind", "setup failed"); goto done; }

    char before[33], after[33];
    entry_md5(usa, size, mario, before);
    if (sigil_gamecube_bind_serial(usa, size, mario->first_block) != SIGIL_OK) fail("bind", "binding another save failed");
    entry_md5(usa, size, mario, after);
    if (strcmp(before, after) != 0) fail("bind", "binding changed a save that doesn't bind");

    sigil_gamecube_format(ours, CARD_2MIB, false);
    if (sigil_gamecube_inject(ours, CARD_2MIB, saves.gci[fz_index], saves.len[fz_index]) != SIGIL_OK ||
        sigil_gamecube_card_list(ours, CARD_2MIB, &ours_l) != SIGIL_OK || ours_l->entry_count != 1 ||
        sigil_gamecube_bind_serial(ours, CARD_2MIB, ours_l->entries[0].first_block) != SIGIL_OK ||
        sigil_gamecube_extract(ours, CARD_2MIB, ours_l->entries[0].first_block, 4, moved) != SIGIL_OK) {
        fail("bind", "binding to a sigil card failed");
        goto done;
    }
    if (memcmp(moved, saves.gci[fz_index], saves.len[fz_index]) == 0) fail("bind", "binding to another card left the save unchanged");

    if (sigil_gamecube_delete(usa, size, fz->first_block) != SIGIL_OK ||
        sigil_gamecube_inject(usa, size, moved, saves.len[fz_index]) != SIGIL_OK ||
        sigil_gamecube_card_list(usa, size, &back_l) != SIGIL_OK) {
        fail("bind", "moving the save back failed");
        goto done;
    }
    const sigil_card_entry *back = find_entry(back_l, "f_zero.dat");
    if (!back || sigil_gamecube_bind_serial(usa, size, back->first_block) != SIGIL_OK) {
        fail("bind", "binding back to the USA card failed");
        goto done;
    }
    sigil_card_listing_free(back_l);
    back_l = NULL;
    if (sigil_gamecube_card_list(usa, size, &back_l) == SIGIL_OK) {
        check_expected_entries("bind", "card-raw-usa", "memcard-image.raw", entries, back_l, usa, size, NULL);
    }

done:
    free(moved);
    save_set_free(&saves);
    sigil_card_listing_free(back_l);
    sigil_card_listing_free(ours_l);
    sigil_card_listing_free(l);
    free(ours);
    free(usa);
}

int main(void) {
    char path[1024];
    corpus_table manifest, entries;
    if (corpus_platform_path("ngc", "manifest.tsv", path, sizeof(path)) != 0 ||
        corpus_load(path, &manifest) != 0) {
        fprintf(stderr, "SKIP: no ngc manifest\n");
        return TEST_SKIP;
    }
    if (corpus_platform_path("ngc", "entries.tsv", path, sizeof(path)) != 0 ||
        corpus_load(path, &entries) != 0) {
        corpus_free(&manifest);
        fprintf(stderr, "SKIP: no ngc entries\n");
        return TEST_SKIP;
    }

    for (size_t r = 0; r < manifest.nrows; r++) {
        const char *kind = corpus_get(&manifest, r, "kind");
        const char *id = corpus_get(&manifest, r, "id");
        const char *file = corpus_get(&manifest, r, "path");
        if (!kind || !id || !file) continue;
        if (strcmp(kind, "card") == 0) {
            check_card(id, file, &entries);
        } else if (strcmp(kind, "gci") == 0 || strcmp(kind, "wrapped") == 0) {
            check_not_card(id, file);
            check_save_sample(id, file, kind, &manifest, &entries);
        }
    }
    check_wrappers_agree(&manifest);
    check_format_matches_console(&manifest);
    check_same_identity_refused(&manifest);
    check_no_room_for_blocks(&manifest);
    check_no_room_in_directory(&manifest);
    check_delete(&manifest, &entries);
    check_copy_choice(&manifest, &entries);
    check_shared_chain(&manifest);
    check_bind_serial(&manifest, &entries);
    int missing = g_checks ? corpus_count_missing(&manifest, "ngc") : 0;
    corpus_free(&manifest);
    corpus_free(&entries);

    printf("gamecube cards: %d checks, %d failures\n", g_checks, g_fails);
    if (!g_checks) return TEST_SKIP;
    return corpus_exit(g_fails + missing);
}
