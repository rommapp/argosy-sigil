// SPDX-License-Identifier: MPL-2.0
#include "save_corpus.h"
#include "card_dreamcast.h"
#include <stdbool.h>

#define TEST_SKIP 77

static int g_fails = 0;
static int g_cards = 0;

static void fail(const char *id, const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", id, what);
    g_fails++;
}

static void md5_hex(const uint8_t *data, size_t len, char out[33]) {
    sigil_md5 m;
    uint8_t digest[16];
    sigil_md5_init(&m);
    sigil_md5_update(&m, data, len);
    sigil_md5_final(&m, digest);
    sigil_md5_hex(digest, out);
}

static void entry_md5(const uint8_t *image, const sigil_card_entry *e, char out[33]) {
    out[0] = '\0';
    size_t len = (size_t)e->blocks * VMU_BLOCK_SIZE;
    uint8_t *data = (uint8_t *)malloc(len);
    if (!data) return;
    if (sigil_dreamcast_entry_data(image, e->first_block, e->blocks, data) == SIGIL_OK) md5_hex(data, len, out);
    free(data);
}

static const sigil_card_entry *find_entry(const sigil_card_listing *l, const char *name) {
    for (size_t i = 0; i < l->entry_count; i++) {
        if (strcmp(l->entries[i].name, name) == 0) return &l->entries[i];
    }
    return NULL;
}

static const char *expected_md5(const corpus_table *entries, const char *id, const char *name) {
    for (size_t r = 0; r < entries->nrows; r++) {
        if (strcmp(corpus_get(entries, r, "id"), id) == 0 && strcmp(corpus_get(entries, r, "entry"), name) == 0) {
            return corpus_get(entries, r, "data_md5");
        }
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
        if (strcmp(corpus_get(entries, r, "owner_id"), "-") != 0 || e->owner_id[0] != '\0') fail(label, "owner_id not empty");
        if (strtoul(corpus_get(entries, r, "blocks"), NULL, 10) != e->blocks) fail(label, "block count differs");

        char md5[33];
        entry_md5(image, e, md5);
        if (strcmp(md5, corpus_get(entries, r, "data_md5")) != 0) fail(label, "data md5 differs");
    }
    if (expected != listing->entry_count) fail(where, "listing holds entries entries.tsv doesn't");
}

typedef struct {
    const char *id;
    uint32_t    total_blocks;
    uint32_t    free_blocks;
    uint32_t    free_slots;
} card_fact;

/* total_blocks is the u16 at root offset 0x50. free_blocks counts 0xFFFC
 * FAT words below it; free_slots counts type-0x00 entries among the 208 slots
 * of blocks 241-253. All read from hex dumps. chao-adv2 also holds a 61-block
 * chain, 239 down to 179, that no directory entry names. */
static const card_fact CARD_FACTS[] = {
    { "gundam-0079-flycast",     200, 194, 207 },
    { "vmu-a1-nine-games",       200, 156, 199 },
    { "jsr-forward-dir-vmu",     240, 130, 203 },
    { "extended-blocks-vmu",     241, 233, 207 },
    { "chao-adv2-game-vmu",      240,  51, 207 },
    { "pacit-game-and-data-vmu", 200, 183, 206 },
    { "vmoooo-vmu",              200,  72, 207 },
    { "empty-vmu",               200, 200, 208 },
};

static void check_facts(const char *id, const sigil_card_listing *listing) {
    bool known = false;
    for (size_t i = 0; i < sizeof(CARD_FACTS) / sizeof(CARD_FACTS[0]); i++) {
        if (strcmp(CARD_FACTS[i].id, id) != 0) continue;
        known = true;
        if (listing->total_blocks != CARD_FACTS[i].total_blocks) fail(id, "total block count differs");
        if (listing->free_blocks != CARD_FACTS[i].free_blocks) fail(id, "free block count differs");
        if (listing->free_slots != CARD_FACTS[i].free_slots) fail(id, "free slot count differs");
    }
    if (!known) fail(id, "no recorded card facts");
    if (listing->corrupt_count != 0) fail(id, "corrupt count differs");
    if (listing->format != SIGIL_CARD_FORMAT_DREAMCAST_VMU) fail(id, "format differs");
}

/* Every file on the VMU, in directory order, as .dci buffers. */
typedef struct {
    uint8_t *dci[VMU_BLOCKS];
    size_t   len[VMU_BLOCKS];
    size_t   count;
} save_set;

static void save_set_free(save_set *s) {
    for (size_t i = 0; i < s->count; i++) free(s->dci[i]);
    s->count = 0;
}

static uint8_t *extract_entry(const uint8_t *image, const sigil_card_entry *e, size_t *len) {
    *len = sigil_dreamcast_dci_size(e->blocks);
    uint8_t *dci = (uint8_t *)malloc(*len);
    if (dci && sigil_dreamcast_extract(image, e->first_block, e->blocks, dci) == SIGIL_OK) return dci;
    free(dci);
    return NULL;
}

static bool extract_all(const uint8_t *image, const sigil_card_listing *l, save_set *out) {
    out->count = 0;
    for (size_t i = 0; i < l->entry_count; i++) {
        uint8_t *dci = extract_entry(image, &l->entries[i], &out->len[out->count]);
        if (!dci) { save_set_free(out); return false; }
        out->dci[out->count++] = dci;
    }
    return true;
}

static bool build_card(const save_set *saves, uint8_t *image) {
    sigil_dreamcast_format(image);
    for (size_t i = 0; i < saves->count; i++) {
        if (sigil_dreamcast_inject(image, saves->dci[i], saves->len[i]) != SIGIL_OK) return false;
    }
    return true;
}

/* Rebuilding a VMU from its own files keeps every file's bytes, builds the
 * same VMU twice, and leaves each file verifiable. */
static void check_rebuild(const char *id, const char *path, const corpus_table *entries,
                          const uint8_t *image, const sigil_card_listing *listing) {
    save_set saves;
    if (!extract_all(image, listing, &saves)) { fail(id, "extract failed"); return; }
    for (size_t i = 0; i < saves.count; i++) {
        if (saves.dci[i][2] != 0 || saves.dci[i][3] != 0) fail(id, "an extracted .dci keeps its first block");
    }

    uint8_t *built = (uint8_t *)malloc(VMU_CARD_SIZE);
    uint8_t *again = (uint8_t *)malloc(VMU_CARD_SIZE);
    sigil_card_listing *rebuilt = NULL;
    if (!built || !again || !build_card(&saves, built) || !build_card(&saves, again)) {
        fail(id, "rebuild failed");
    } else if (memcmp(built, again, VMU_CARD_SIZE) != 0) {
        fail(id, "two builds from the same saves differ");
    } else if (sigil_dreamcast_card_list(built, &rebuilt) != SIGIL_OK) {
        fail(id, "rebuilt VMU did not list");
    } else {
        char label[256];
        snprintf(label, sizeof(label), "%s (rebuilt)", id);
        check_expected_entries(label, id, path, entries, rebuilt, built);
        for (size_t i = 0; i < saves.count; i++) {
            if (sigil_dreamcast_verify(built, saves.dci[i], saves.len[i]) != SIGIL_OK) fail(label, "a save did not verify");
        }
    }
    sigil_card_listing_free(rebuilt);
    free(built);
    free(again);
    save_set_free(&saves);
}

static void check_card(const char *id, const char *path, const corpus_table *entries) {
    char full[1024];
    if (corpus_sample_path("dc", id, path, full, sizeof(full)) != 0) return;
    sigil_io *io = sigil_io_open_file(full);
    if (!io) return;
    g_cards++;

    uint8_t *image = (uint8_t *)malloc(VMU_CARD_SIZE);
    sigil_card_listing *listing = NULL;
    if (!image || sigil_dreamcast_card_load(io, image) != SIGIL_OK ||
        sigil_dreamcast_card_list(image, &listing) != SIGIL_OK) {
        fail(id, "VMU did not load");
    } else {
        check_expected_entries(id, id, path, entries, listing, image);
        check_facts(id, listing);
        check_rebuild(id, path, entries, image, listing);
        sigil_card_listing *public_listing = NULL;
        if (sigil_card_list(io, &public_listing) != SIGIL_OK ||
            public_listing->format != SIGIL_CARD_FORMAT_DREAMCAST_VMU ||
            public_listing->entry_count != listing->entry_count) {
            fail(id, "sigil_card_list disagrees with the internal listing");
        }
        sigil_card_listing_free(public_listing);
    }
    sigil_card_listing_free(listing);
    free(image);
    sigil_io_close(io);
}

static void check_not_card(const char *platform, const char *id, const char *path) {
    char full[1024];
    if (corpus_sample_path(platform, id, path, full, sizeof(full)) != 0) return;
    sigil_io *io = sigil_io_open_file(full);
    if (!io) return;
    sigil_card_listing *listing = NULL;
    if (sigil_card_list(io, &listing) != SIGIL_ERR_UNSUPPORTED_FORMAT) fail(id, "read as a card");
    sigil_card_listing_free(listing);
    sigil_io_close(io);
}

/* PS1 cards are 128 KiB too; none may pass as a VMU. */
static void check_ps1_cards_not_vmu(void) {
    char path[1024];
    corpus_table psx;
    if (corpus_platform_path("psx", "manifest.tsv", path, sizeof(path)) != 0 || corpus_load(path, &psx) != 0) return;
    uint8_t *image = (uint8_t *)malloc(VMU_CARD_SIZE);
    for (size_t r = 0; image && r < psx.nrows; r++) {
        if (strcmp(corpus_get(&psx, r, "kind"), "card") != 0) continue;
        const char *id = corpus_get(&psx, r, "id");
        char full[1024];
        if (corpus_sample_path("psx", id, corpus_get(&psx, r, "path"), full, sizeof(full)) != 0) continue;
        sigil_io *io = sigil_io_open_file(full);
        if (!io) continue;
        sigil_card_listing *listing = NULL;
        if (sigil_dreamcast_card_load(io, image) != SIGIL_ERR_UNSUPPORTED_FORMAT) fail(id, "a PS1 card loaded as a VMU");
        if (sigil_card_list(io, &listing) == SIGIL_OK && listing->format == SIGIL_CARD_FORMAT_DREAMCAST_VMU) {
            fail(id, "sigil_card_list reported a PS1 card as a VMU");
        }
        sigil_card_listing_free(listing);
        sigil_io_close(io);
    }
    free(image);
    corpus_free(&psx);
}

static uint8_t *load_sample_bytes(const corpus_table *manifest, const char *id, const char *path, size_t *len) {
    for (size_t r = 0; r < manifest->nrows; r++) {
        if (strcmp(corpus_get(manifest, r, "id"), id) != 0) continue;
        if (path && strcmp(corpus_get(manifest, r, "path"), path) != 0) continue;
        char full[1024];
        if (corpus_sample_path("dc", id, corpus_get(manifest, r, "path"), full, sizeof(full)) != 0) return NULL;
        return corpus_read_file(full, len);
    }
    return NULL;
}

static bool load_sample(const corpus_table *manifest, const char *id, uint8_t *image) {
    size_t len = 0;
    uint8_t *bytes = load_sample_bytes(manifest, id, NULL, &len);
    bool ok = bytes && len == VMU_CARD_SIZE;
    if (ok) memcpy(image, bytes, VMU_CARD_SIZE);
    free(bytes);
    return ok;
}

/* Extracts the file named `name` from sample `id`. */
static uint8_t *sample_entry(const corpus_table *manifest, const char *id, const char *name, size_t *len) {
    uint8_t *image = (uint8_t *)malloc(VMU_CARD_SIZE);
    sigil_card_listing *l = NULL;
    uint8_t *dci = NULL;
    if (image && load_sample(manifest, id, image) && sigil_dreamcast_card_list(image, &l) == SIGIL_OK) {
        const sigil_card_entry *e = find_entry(l, name);
        if (e) dci = extract_entry(image, e, len);
    }
    sigil_card_listing_free(l);
    free(image);
    return dci;
}

#define ROOT_OFFSET (255u * VMU_BLOCK_SIZE)
#define FAT_OFFSET  (254u * VMU_BLOCK_SIZE)

/* A formatted VMU matches empty-vmu except where that image carries its
 * writer's own choices. From its hex dump: root 0x10-0x14 is custom colour
 * 01 ab cd ef 42, 0x30-0x37 the time 2025-10-18 13:42:00, 0x4e icon shape
 * 0x2a and 0x52 the word 0x29; FAT words for blocks 200-240 are 0x0000.
 * sigil takes those from the stock image instead: its whole root block
 * equals pacit-game-and-data-vmu's, and blocks 200-240 read 0xfffc there and
 * in gundam-0079-flycast. */
static bool format_difference_allowed(size_t off) {
    if (off >= ROOT_OFFSET + 0x10 && off <= ROOT_OFFSET + 0x14) return true;
    if (off >= ROOT_OFFSET + 0x30 && off <= ROOT_OFFSET + 0x37) return true;
    if (off == ROOT_OFFSET + 0x4E || off == ROOT_OFFSET + 0x52) return true;
    return off >= FAT_OFFSET + 2 * 200 && off < FAT_OFFSET + 2 * 241;
}

static void check_format(const corpus_table *manifest) {
    size_t empty_len = 0, pacit_len = 0;
    uint8_t *empty = load_sample_bytes(manifest, "empty-vmu", NULL, &empty_len);
    uint8_t *pacit = load_sample_bytes(manifest, "pacit-game-and-data-vmu", NULL, &pacit_len);
    uint8_t *image = (uint8_t *)malloc(VMU_CARD_SIZE);
    if (image) sigil_dreamcast_format(image);
    if (image && empty && empty_len == VMU_CARD_SIZE) {
        size_t differing = 0;
        for (size_t i = 0; i < VMU_CARD_SIZE; i++) {
            if (image[i] != empty[i] && !format_difference_allowed(i)) differing++;
        }
        if (differing) fail("empty-vmu", "a formatted VMU differs from the empty sample outside the writer's own fields");
        if (image[FAT_OFFSET + 2 * 200] != 0xFC || image[FAT_OFFSET + 2 * 200 + 1] != 0xFF) {
            fail("empty-vmu", "block 200 is not marked free");
        }
    }
    if (image && pacit && pacit_len == VMU_CARD_SIZE &&
        memcmp(image + ROOT_OFFSET, pacit + ROOT_OFFSET, VMU_BLOCK_SIZE) != 0) {
        fail("pacit-game-and-data-vmu", "a formatted root block differs from the stock one");
    }
    free(image);
    free(pacit);
    free(empty);
}

/* Injects `dci` into a formatted VMU, checks the one listed entry, and
 * extracts it again. */
static void check_round_trip(const char *id, const corpus_table *entries, const uint8_t *dci, size_t len,
                             const char *name, uint32_t first_block) {
    uint8_t *image = (uint8_t *)malloc(VMU_CARD_SIZE);
    uint8_t *back = (uint8_t *)malloc(len);
    sigil_card_listing *l = NULL;
    if (image && back) {
        sigil_dreamcast_format(image);
        const sigil_card_entry *e = NULL;
        if (sigil_dreamcast_inject(image, dci, len) != SIGIL_OK ||
            sigil_dreamcast_card_list(image, &l) != SIGIL_OK || l->entry_count != 1 ||
            !(e = find_entry(l, name)) ||
            sigil_dreamcast_extract(image, e->first_block, e->blocks, back) != SIGIL_OK) {
            fail(id, "inject or extract failed");
        } else {
            char md5[33];
            entry_md5(image, e, md5);
            const char *want = expected_md5(entries, id, name);
            if (memcmp(back, dci, len) != 0) fail(id, "the save changed on its way through a VMU");
            if (!want || strcmp(md5, want) != 0) fail(id, "data md5 differs");
            if (e->first_block != first_block) fail(id, "first block differs");
            if (sigil_dreamcast_verify(image, dci, len) != SIGIL_OK) fail(id, "save did not verify");
        }
    }
    sigil_card_listing_free(l);
    free(back);
    free(image);
}

/* Data files fill the 200-block user area from block 199 down, where the
 * first data file sits on the gundam-0079-flycast, vmu-a1-nine-games and
 * pacit-game-and-data-vmu samples. */
static void check_dci_round_trip(const corpus_table *manifest, const corpus_table *entries) {
    size_t len = 0;
    uint8_t *dci = load_sample_bytes(manifest, "project-justice-dci", NULL, &len);
    if (!dci) return;
    check_round_trip("project-justice-dci", entries, dci, len, "PJUSTICE_SYS", 199);
    free(dci);
}

/* The directory entry IKARUGA.VMI describes, written out by hand from its hex
 * dump: data file (mode 0x0000), not protected, name IKARUGA_DATA, time
 * d2 07 09 07 14 16 06 05 (2002-09-07 20:22:06, weekday 5) in BCD, and
 * 0x4400 bytes = 34 blocks. */
static const uint8_t IKARUGA_ENTRY[VMU_DIR_ENTRY_SIZE] = {
    0x33, 0x00, 0x00, 0x00, 'I', 'K', 'A', 'R', 'U', 'G', 'A', '_', 'D', 'A', 'T', 'A',
    0x20, 0x02, 0x09, 0x07, 0x20, 0x22, 0x06, 0x05, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static void check_vms_round_trip(const corpus_table *manifest, const corpus_table *entries) {
    size_t vms_len = 0, vmi_len = 0;
    uint8_t *vms = load_sample_bytes(manifest, "ikaruga-vms-vmi", "IKARUGA.VMS", &vms_len);
    uint8_t *vmi = load_sample_bytes(manifest, "ikaruga-vms-vmi", "IKARUGA.VMI", &vmi_len);
    size_t len = sigil_dreamcast_dci_size((uint32_t)(vms_len / VMU_BLOCK_SIZE));
    uint8_t *dci = (uint8_t *)malloc(len);
    if (vms && vmi && dci) {
        if (sigil_dreamcast_dci_from_vms(vms, vms_len, vmi, vmi_len, dci) != SIGIL_OK) {
            fail("ikaruga-vms-vmi", "VMS and VMI did not convert");
        } else {
            if (memcmp(dci, IKARUGA_ENTRY, VMU_DIR_ENTRY_SIZE) != 0) fail("ikaruga-vms-vmi", "directory entry differs");
            check_round_trip("ikaruga-vms-vmi", entries, dci, len, "IKARUGA_DATA", 199);
        }
        vmi[0] ^= 0x01;
        if (sigil_dreamcast_dci_from_vms(vms, vms_len, vmi, vmi_len, dci) != SIGIL_ERR_UNSUPPORTED_FORMAT) {
            fail("ikaruga-vms-vmi", "a VMI with a bad checksum converted");
        }
    }
    free(dci);
    free(vmi);
    free(vms);
}

/* A game file goes at block 0; a second game file or a repeated name is
 * refused without touching the VMU. */
static void check_game_rules(const corpus_table *manifest) {
    size_t game_len = 0, other_len = 0, data_len = 0;
    uint8_t *game = sample_entry(manifest, "pacit-game-and-data-vmu", "PACIT_NM.VMU", &game_len);
    uint8_t *other = sample_entry(manifest, "chao-adv2-game-vmu", "SONIC2____VM", &other_len);
    uint8_t *data = sample_entry(manifest, "pacit-game-and-data-vmu", "NAMCOMUS.SYS", &data_len);
    uint8_t *image = (uint8_t *)malloc(VMU_CARD_SIZE);
    uint8_t *copy = (uint8_t *)malloc(VMU_CARD_SIZE);
    sigil_card_listing *l = NULL;
    if (game && other && data && image && copy) {
        sigil_dreamcast_format(image);
        if (sigil_dreamcast_inject(image, data, data_len) != SIGIL_OK ||
            sigil_dreamcast_inject(image, game, game_len) != SIGIL_OK ||
            sigil_dreamcast_card_list(image, &l) != SIGIL_OK) {
            fail("pacit-game-and-data-vmu", "game inject failed");
        } else {
            const sigil_card_entry *e = find_entry(l, "PACIT_NM.VMU");
            if (!e || e->first_block != 0) fail("pacit-game-and-data-vmu", "game file does not start at block 0");
            memcpy(copy, image, VMU_CARD_SIZE);
            if (sigil_dreamcast_inject(image, other, other_len) != SIGIL_ERR_NO_SPACE) {
                fail("chao-adv2-game-vmu", "a second game file went in");
            }
            if (sigil_dreamcast_inject(image, data, data_len) != SIGIL_ERR_EXISTS) {
                fail("pacit-game-and-data-vmu", "a repeated name went in");
            }
            if (memcmp(copy, image, VMU_CARD_SIZE) != 0) fail("pacit-game-and-data-vmu", "a refused inject changed the VMU");
        }
    }
    sigil_card_listing_free(l);
    free(copy);
    free(image);
    free(data);
    free(other);
    free(game);
}

/* chao-adv2 has 51 free blocks; the 61-block JETSET___XLA doesn't fit. */
static void check_full_card_refuses(const corpus_table *manifest) {
    size_t len = 0;
    uint8_t *dci = sample_entry(manifest, "jsr-forward-dir-vmu", "JETSET___XLA", &len);
    uint8_t *image = (uint8_t *)malloc(VMU_CARD_SIZE);
    uint8_t *copy = (uint8_t *)malloc(VMU_CARD_SIZE);
    if (dci && image && copy && load_sample(manifest, "chao-adv2-game-vmu", image)) {
        memcpy(copy, image, VMU_CARD_SIZE);
        if (sigil_dreamcast_inject(image, dci, len) != SIGIL_ERR_NO_SPACE) fail("chao-adv2-game-vmu", "full VMU took a save");
        if (memcmp(copy, image, VMU_CARD_SIZE) != 0) fail("chao-adv2-game-vmu", "a refused inject changed the VMU");
    }
    free(copy);
    free(image);
    free(dci);
}

/* Deleting CVS.S2___SYS (12 blocks) frees its blocks and slot and leaves the
 * other eight files intact; the freed room takes the file back. */
static void check_delete(const corpus_table *manifest) {
    uint8_t *image = (uint8_t *)malloc(VMU_CARD_SIZE);
    if (!image || !load_sample(manifest, "vmu-a1-nine-games", image)) { free(image); return; }
    sigil_card_listing *before = NULL, *after = NULL;
    save_set kept = {0};
    size_t len = 0;
    uint8_t *deleted = NULL;
    if (sigil_dreamcast_card_list(image, &before) != SIGIL_OK || !extract_all(image, before, &kept)) {
        fail("vmu-a1-nine-games", "list or extract failed");
    } else {
        const sigil_card_entry *e = find_entry(before, "CVS.S2___SYS");
        if (!e || !(deleted = extract_entry(image, e, &len)) ||
            sigil_dreamcast_delete(image, e->first_block) != SIGIL_OK ||
            sigil_dreamcast_card_list(image, &after) != SIGIL_OK) {
            fail("vmu-a1-nine-games", "delete failed");
        } else {
            if (find_entry(after, "CVS.S2___SYS")) fail("vmu-a1-nine-games", "deleted file still listed");
            if (after->free_blocks != before->free_blocks + 12) fail("vmu-a1-nine-games", "delete did not free the blocks");
            if (after->free_slots != before->free_slots + 1) fail("vmu-a1-nine-games", "delete did not free the slot");
            size_t others = 0;
            for (size_t i = 0; i < kept.count; i++) {
                if (memcmp(kept.dci[i] + 4, "CVS.S2___SYS", VMU_NAME_LEN) == 0) continue;
                others++;
                if (sigil_dreamcast_verify(image, kept.dci[i], kept.len[i]) != SIGIL_OK) {
                    fail("vmu-a1-nine-games", "delete touched another file");
                }
            }
            if (others != 8) fail("vmu-a1-nine-games", "other file count differs");
            if (sigil_dreamcast_verify(image, deleted, len) != SIGIL_ERR_NOT_FOUND) {
                fail("vmu-a1-nine-games", "deleted file still verifies");
            }
            if (sigil_dreamcast_inject(image, deleted, len) != SIGIL_OK ||
                sigil_dreamcast_verify(image, deleted, len) != SIGIL_OK) {
                fail("vmu-a1-nine-games", "freed room did not take the file back");
            }
        }
    }
    free(deleted);
    save_set_free(&kept);
    sigil_card_listing_free(before);
    sigil_card_listing_free(after);
    free(image);
}

/* gundam-0079-flycast's one file, GUNDAM_US_01, has its directory entry at
 * 0x1fa00 and starts at block 199 (hex dump). Pointing that block's FAT word
 * at itself or at a free marker, or recording 5 blocks for its 6-block chain,
 * leaves it corrupt. */
static void check_broken_chain(const corpus_table *manifest) {
    uint8_t *image = (uint8_t *)malloc(VMU_CARD_SIZE);
    if (!image || !load_sample(manifest, "gundam-0079-flycast", image)) { free(image); return; }
    static const struct { size_t off; uint8_t bytes[2]; } BREAKS[] = {
        { FAT_OFFSET + 2 * 199, { 199, 0x00 } },
        { FAT_OFFSET + 2 * 199, { 0xFC, 0xFF } },
        { 0x1FA00 + 0x18,       { 5, 0x00 } },
    };
    for (size_t i = 0; i < sizeof(BREAKS) / sizeof(BREAKS[0]); i++) {
        uint8_t original[2];
        memcpy(original, image + BREAKS[i].off, 2);
        memcpy(image + BREAKS[i].off, BREAKS[i].bytes, 2);
        sigil_card_listing *l = NULL;
        if (sigil_dreamcast_card_list(image, &l) != SIGIL_OK || l->entry_count != 0 || l->corrupt_count != 1) {
            fail("gundam-0079-flycast", "a broken chain was listed");
        }
        sigil_card_listing_free(l);
        memcpy(image + BREAKS[i].off, original, 2);
    }
    image[ROOT_OFFSET] = 0x54;
    sigil_card_listing *l = NULL;
    if (sigil_dreamcast_card_list(image, &l) != SIGIL_ERR_UNSUPPORTED_FORMAT) {
        fail("gundam-0079-flycast", "a VMU without the format fill listed");
    }
    sigil_card_listing_free(l);
    free(image);
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

/* A real VMU with one byte added or cut is no VMU, whether or not the stream
 * reports its size. */
static void check_wrong_size(const corpus_table *manifest) {
    uint8_t *bytes = (uint8_t *)calloc(1, VMU_CARD_SIZE + 1);
    if (!bytes || !load_sample(manifest, "gundam-0079-flycast", bytes)) { free(bytes); return; }
    static const size_t SIZES[] = { VMU_CARD_SIZE - 1, VMU_CARD_SIZE + 1 };
    for (size_t i = 0; i < 2; i++) {
        for (int sized = 0; sized < 2; sized++) {
            mem_ctx m = { bytes, SIZES[i] };
            sigil_io io = { mem_read, sized ? mem_size : NULL, NULL, &m };
            sigil_card_listing *l = NULL;
            if (sigil_card_list(&io, &l) != SIGIL_ERR_UNSUPPORTED_FORMAT) fail("gundam-0079-flycast", "a resized VMU listed");
            sigil_card_listing_free(l);
        }
    }
    mem_ctx m = { bytes, VMU_CARD_SIZE };
    sigil_io io = { mem_read, NULL, NULL, &m };
    sigil_card_listing *l = NULL;
    if (sigil_card_list(&io, &l) != SIGIL_OK) fail("gundam-0079-flycast", "an unsized stream of the VMU did not list");
    sigil_card_listing_free(l);
    free(bytes);
}

int main(void) {
    char path[1024];
    corpus_table manifest, entries;
    if (corpus_platform_path("dc", "manifest.tsv", path, sizeof(path)) != 0 ||
        corpus_load(path, &manifest) != 0) {
        fprintf(stderr, "SKIP: no dc manifest\n");
        return TEST_SKIP;
    }
    if (corpus_platform_path("dc", "entries.tsv", path, sizeof(path)) != 0 ||
        corpus_load(path, &entries) != 0) {
        corpus_free(&manifest);
        fprintf(stderr, "SKIP: no dc entries\n");
        return TEST_SKIP;
    }

    for (size_t r = 0; r < manifest.nrows; r++) {
        const char *kind = corpus_get(&manifest, r, "kind");
        const char *id = corpus_get(&manifest, r, "id");
        const char *file = corpus_get(&manifest, r, "path");
        if (!kind || !id || !file) continue;
        if (strcmp(kind, "vmu") == 0) check_card(id, file, &entries);
        else check_not_card("dc", id, file);
    }
    check_ps1_cards_not_vmu();
    check_format(&manifest);
    check_dci_round_trip(&manifest, &entries);
    check_vms_round_trip(&manifest, &entries);
    check_game_rules(&manifest);
    check_full_card_refuses(&manifest);
    check_delete(&manifest);
    check_broken_chain(&manifest);
    check_wrong_size(&manifest);
    int missing = g_cards ? corpus_count_missing(&manifest, "dc") : 0;
    corpus_free(&manifest);
    corpus_free(&entries);

    printf("dreamcast VMUs: %d checked, %d failures\n", g_cards, g_fails);
    if (!g_cards) return TEST_SKIP;
    return corpus_exit(g_fails + missing);
}
