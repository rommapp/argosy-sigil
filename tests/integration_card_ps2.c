// SPDX-License-Identifier: MPL-2.0
#include "save_corpus.h"
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "card_ps2.h"
#include <stdbool.h>
#include <time.h>

#define TEST_SKIP 77

static int g_fails = 0;
static int g_cards = 0;

static void fail(const char *where, const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", where, what);
    g_fails++;
}

static void expected_owner(const char *raw, char out[SIGIL_CARD_OWNER_MAX]) {
    out[0] = '\0';
    if (strcmp(raw, "-") == 0 || strlen(raw) != 10) return;
    memcpy(out, raw, 11);
    out[4] = '-';
}

static void md5_hex(const uint8_t *data, size_t len, char out[33]) {
    sigil_md5 m;
    uint8_t digest[16];
    sigil_md5_init(&m);
    sigil_md5_update(&m, data, len);
    sigil_md5_final(&m, digest);
    sigil_md5_hex(digest, out);
}

static const sigil_card_entry *find_entry(const sigil_card_listing *l, const char *name) {
    for (size_t i = 0; i < l->entry_count; i++) {
        if (strcmp(l->entries[i].name, name) == 0) return &l->entries[i];
    }
    return NULL;
}

static void check_expected_entries(const char *id, const char *path, const corpus_table *entries,
                                   const sigil_card_listing *listing, const sigil_ps2_card *card) {
    size_t expected = 0;
    for (size_t r = 0; r < entries->nrows; r++) {
        if (strcmp(corpus_get(entries, r, "id"), id) != 0) continue;
        if (strcmp(corpus_get(entries, r, "path"), path) != 0) continue;
        expected++;

        const char *name = corpus_get(entries, r, "entry");
        char label[256];
        snprintf(label, sizeof(label), "%s %s", id, name);
        const sigil_card_entry *e = find_entry(listing, name);
        if (!e) { fail(label, "entry not listed"); continue; }

        char owner[SIGIL_CARD_OWNER_MAX];
        expected_owner(corpus_get(entries, r, "owner_id"), owner);
        if (strcmp(owner, e->owner_id) != 0) fail(label, "owner_id differs");
        if (strtoul(corpus_get(entries, r, "blocks"), NULL, 10) != e->blocks) fail(label, "block count differs");

        uint8_t *data = NULL;
        size_t len = 0;
        char md5[33] = "";
        if (sigil_ps2_save_data(card, e->first_block, &data, &len) == SIGIL_OK) md5_hex(data, len, md5);
        free(data);
        if (strcmp(md5, corpus_get(entries, r, "data_md5")) != 0) fail(label, "data md5 differs");
    }
    if (expected != listing->entry_count) fail(id, "listing holds entries entries.tsv doesn't");
}

static void check_card(const char *id, const char *path, const corpus_table *entries) {
    char full[1024];
    if (corpus_sample_path("ps2", id, path, full, sizeof(full)) != 0) return;
    sigil_io *io = sigil_io_open_file(full);
    if (!io) return;
    g_cards++;

    sigil_ps2_card card;
    sigil_card_listing *listing = NULL;
    if (sigil_ps2_card_load(io, &card) != SIGIL_OK) {
        fail(id, "card did not load");
    } else {
        if (sigil_ps2_card_list(&card, &listing) != SIGIL_OK) {
            fail(id, "card did not list");
        } else {
            if (!card.ecc) fail(id, "an 8 MB card with spare bytes read as ECC-less");
            check_expected_entries(id, path, entries, listing, &card);
            sigil_card_listing *public_listing = NULL;
            if (sigil_card_list(io, &public_listing) != SIGIL_OK ||
                public_listing->format != SIGIL_CARD_FORMAT_PS2 ||
                public_listing->entry_count != listing->entry_count) {
                fail(id, "sigil_card_list disagrees with the internal listing");
            }
            sigil_card_listing_free(public_listing);
        }
        sigil_ps2_card_free(&card);
    }
    sigil_card_listing_free(listing);
    sigil_io_close(io);
}

static void check_not_card(const char *id, const char *path) {
    char full[1024];
    if (corpus_sample_path("ps2", id, path, full, sizeof(full)) != 0) return;
    sigil_io *io = sigil_io_open_file(full);
    if (!io) return;
    sigil_card_listing *listing = NULL;
    if (sigil_card_list(io, &listing) != SIGIL_ERR_UNSUPPORTED_FORMAT) fail(id, "a single save read as a card");
    sigil_card_listing_free(listing);
    sigil_io_close(io);
}

static uint8_t *load_sample_bytes(const corpus_table *manifest, const char *id, size_t *len) {
    for (size_t r = 0; r < manifest->nrows; r++) {
        if (strcmp(corpus_get(manifest, r, "id"), id) != 0) continue;
        char full[1024];
        if (corpus_sample_path("ps2", id, corpus_get(manifest, r, "path"), full, sizeof(full)) != 0) return NULL;
        return corpus_read_file(full, len);
    }
    return NULL;
}

/* mymc formatted mymc-mc02; its root directory carries this timestamp
 * (bytes 0x08-0x0F of the "." entry in cluster 41, read by hex dump). */
static const uint8_t MC02_TOD[PS2_TOD_SIZE] = { 0x00, 0x0F, 0x14, 0x05, 0x16, 0x04, 0xE2, 0x07 };

static void check_format_matches_mymc(const corpus_table *manifest) {
    size_t len = 0;
    uint8_t *blank = load_sample_bytes(manifest, "mymc-mc02", &len);
    if (!blank) return;
    sigil_ps2_card card;
    uint8_t *written = NULL;
    if (sigil_ps2_card_format(&card, MC02_TOD) != SIGIL_OK) {
        fail("mymc-mc02", "format failed");
    } else {
        size_t size = sigil_ps2_card_file_size(&card);
        written = (uint8_t *)malloc(size);
        if (!written || size != len || sigil_ps2_card_write(&card, written) != SIGIL_OK) {
            fail("mymc-mc02", "formatted card has the wrong size");
        } else if (memcmp(written, blank, len) != 0) {
            size_t at = 0;
            while (at < len && written[at] == blank[at]) at++;
            fprintf(stderr, "first difference at byte 0x%zx\n", at);
            fail("mymc-mc02", "a formatted card differs from mymc's blank card");
        }
        sigil_ps2_card_free(&card);
    }
    free(written);
    free(blank);
}

/* A folder card's superblock is the first erase block of mymc's blank card,
 * pages without their spare bytes, and PCSX2 reads it as formatted. */
static void check_folder_superblock(const corpus_table *manifest) {
    uint8_t super[PS2_FOLDER_SUPERBLOCK_SIZE];
    sigil_ps2_folder_superblock(super);
    if (!sigil_ps2_folder_superblock_usable(super, sizeof(super))) fail("folder superblock", "PCSX2 would read it as unformatted");

    size_t len = 0;
    uint8_t *blank = load_sample_bytes(manifest, "mymc-mc02", &len);
    if (blank) {
        const size_t stride = 512 + 16;
        for (size_t page = 0; page < PS2_FOLDER_SUPERBLOCK_SIZE / 512; page++) {
            if (len < (page + 1) * stride || memcmp(super + page * 512, blank + page * stride, 512) != 0) {
                fail("folder superblock", "differs from the first block of mymc's blank card");
                break;
            }
        }
        free(blank);
    }

    for (size_t r = 0; r < manifest->nrows; r++) {
        const char *path = corpus_get(manifest, r, "path");
        const char *base = strrchr(path, '/');
        if (strcmp(base ? base + 1 : path, "_pcsx2_superblock") != 0) continue;
        char full[1024];
        size_t real_len = 0;
        uint8_t *real = corpus_sample_path("ps2", corpus_get(manifest, r, "id"), path, full, sizeof(full)) == 0
                            ? corpus_read_file(full, &real_len) : NULL;
        if (!real) continue;
        if (!sigil_ps2_folder_superblock_usable(real, real_len)) fail(corpus_get(manifest, r, "id"), "an emulator's superblock reads as unusable");
        if (real_len < 0x16 || memcmp(real, super, 0x16) != 0) fail(corpus_get(manifest, r, "id"), "an emulator's superblock magic differs from sigil's");
        free(real);
    }

    uint8_t *copy = (uint8_t *)malloc(sizeof(super));
    memcpy(copy, super, sizeof(super));
    if (sigil_ps2_folder_superblock_usable(NULL, 0)) fail("folder superblock", "a missing file reads as usable");
    if (sigil_ps2_folder_superblock_usable(copy, 0)) fail("folder superblock", "an empty file reads as usable");
    if (sigil_ps2_folder_superblock_usable(copy, 512)) fail("folder superblock", "the superblock page alone reads as usable");
    if (sigil_ps2_folder_superblock_usable(copy, sizeof(super) - 1)) fail("folder superblock", "a short file reads as usable");
    copy[0x16] = 0;
    if (sigil_ps2_folder_superblock_usable(copy, sizeof(super))) fail("folder superblock", "an unformatted block reads as usable");
    free(copy);
}

/* Writing a loaded card back reproduces the file, spare bytes included, which
 * pins the ECC against real pages. */
static void check_write_reproduces_file(const corpus_table *manifest) {
    size_t len = 0;
    uint8_t *file = load_sample_bytes(manifest, "mymc-mc01", &len);
    if (!file) return;
    char full[1024];
    sigil_io *io = NULL;
    for (size_t r = 0; r < manifest->nrows && !io; r++) {
        if (strcmp(corpus_get(manifest, r, "id"), "mymc-mc01") != 0) continue;
        if (corpus_sample_path("ps2", "mymc-mc01", corpus_get(manifest, r, "path"), full, sizeof(full)) == 0) {
            io = sigil_io_open_file(full);
        }
    }
    sigil_ps2_card card;
    if (io && sigil_ps2_card_load(io, &card) == SIGIL_OK) {
        uint8_t *written = (uint8_t *)malloc(len);
        if (!written || sigil_ps2_card_file_size(&card) != len || sigil_ps2_card_write(&card, written) != SIGIL_OK ||
            memcmp(written, file, len) != 0) {
            fail("mymc-mc01", "writing the loaded card back changes it");
        }
        free(written);
        sigil_ps2_card_free(&card);
    }
    if (io) sigil_io_close(io);
    free(file);
}

/* mymc built mymc-mc01 by formatting and then writing BEDATA-SYSTEM and
 * BESCES-50501REZ. Its root "." entry carries this creation time (hex dump,
 * cluster 41 bytes 0x08-0x0F). */
static const uint8_t MC01_TOD[PS2_TOD_SIZE] = { 0x00, 0x00, 0x35, 0x17, 0x15, 0x04, 0xE2, 0x07 };

static bool load_card(const corpus_table *manifest, const char *id, sigil_ps2_card *card) {
    for (size_t r = 0; r < manifest->nrows; r++) {
        if (strcmp(corpus_get(manifest, r, "id"), id) != 0) continue;
        char full[1024];
        if (corpus_sample_path("ps2", id, corpus_get(manifest, r, "path"), full, sizeof(full)) != 0) return false;
        sigil_io *io = sigil_io_open_file(full);
        if (!io) return false;
        int rc = sigil_ps2_card_load(io, card);
        sigil_io_close(io);
        return rc == SIGIL_OK;
    }
    return false;
}

static bool build_from(const sigil_ps2_card *source, const sigil_card_listing *listing, sigil_ps2_card *built) {
    if (sigil_ps2_card_format(built, MC01_TOD) != SIGIL_OK) return false;
    bool ok = true;
    for (size_t i = 0; i < listing->entry_count && ok; i++) {
        sigil_ps2_save save;
        ok = sigil_ps2_extract(source, listing->entries[i].first_block, &save) == SIGIL_OK;
        if (ok) {
            ok = sigil_ps2_inject(built, &save) == SIGIL_OK && sigil_ps2_verify(built, &save) == SIGIL_OK &&
                 sigil_ps2_inject(built, &save) == SIGIL_ERR_EXISTS;
            sigil_ps2_save_free(&save);
        }
    }
    return ok;
}

/* Writing a card's saves into a freshly formatted card, in order, lists the
 * same saves with the same bytes, and builds the same file twice. */
static void check_rebuild(const corpus_table *manifest, const corpus_table *entries) {
    sigil_ps2_card source, built, again;
    if (!load_card(manifest, "mymc-mc01", &source)) return;
    sigil_card_listing *listing = NULL, *rebuilt = NULL;
    memset(&built, 0, sizeof(built));
    memset(&again, 0, sizeof(again));
    if (sigil_ps2_card_list(&source, &listing) != SIGIL_OK ||
        !build_from(&source, listing, &built) || !build_from(&source, listing, &again)) {
        fail("mymc-mc01", "rebuild failed, or a duplicate name went in");
    } else if (sigil_ps2_card_list(&built, &rebuilt) != SIGIL_OK) {
        fail("mymc-mc01", "rebuilt card did not list");
    } else {
        check_expected_entries("mymc-mc01", "mc01.ps2", entries, rebuilt, &built);
        size_t size = sigil_ps2_card_file_size(&built);
        uint8_t *a = (uint8_t *)malloc(size), *b = (uint8_t *)malloc(size);
        if (!a || !b || sigil_ps2_card_write(&built, a) != SIGIL_OK || sigil_ps2_card_write(&again, b) != SIGIL_OK ||
            memcmp(a, b, size) != 0) {
            fail("mymc-mc01", "two builds from the same saves differ");
        }
        free(a);
        free(b);
    }
    sigil_card_listing_free(listing);
    sigil_card_listing_free(rebuilt);
    sigil_ps2_card_free(&built);
    sigil_ps2_card_free(&again);
    sigil_ps2_card_free(&source);
}

/* Deleting a save from a real card frees exactly its clusters and leaves the
 * other save intact; writing it back restores it. */
static void check_delete_and_restore(const corpus_table *manifest) {
    sigil_ps2_card card;
    if (!load_card(manifest, "mymc-mc01", &card)) return;
    sigil_card_listing *before = NULL, *after = NULL, *restored = NULL;
    sigil_ps2_save rez, system;
    memset(&rez, 0, sizeof(rez));
    memset(&system, 0, sizeof(system));
    const sigil_card_entry *rez_entry = NULL, *system_entry = NULL;
    if (sigil_ps2_card_list(&card, &before) == SIGIL_OK) {
        rez_entry = find_entry(before, "BESCES-50501REZ");
        system_entry = find_entry(before, "BEDATA-SYSTEM");
    }
    if (!rez_entry || !system_entry ||
        sigil_ps2_extract(&card, rez_entry->first_block, &rez) != SIGIL_OK ||
        sigil_ps2_extract(&card, system_entry->first_block, &system) != SIGIL_OK ||
        sigil_ps2_delete(&card, rez_entry->first_block) != SIGIL_OK ||
        sigil_ps2_card_list(&card, &after) != SIGIL_OK) {
        fail("mymc-mc01", "delete failed");
    } else {
        if (find_entry(after, "BESCES-50501REZ")) fail("mymc-mc01", "deleted save still listed");
        if (after->free_blocks != before->free_blocks + rez_entry->blocks) fail("mymc-mc01", "delete freed the wrong cluster count");
        if (sigil_ps2_verify(&card, &system) != SIGIL_OK) fail("mymc-mc01", "delete touched the other save");
        if (sigil_ps2_inject(&card, &rez) != SIGIL_OK || sigil_ps2_verify(&card, &rez) != SIGIL_OK ||
            sigil_ps2_card_list(&card, &restored) != SIGIL_OK || restored->free_blocks != before->free_blocks) {
            fail("mymc-mc01", "restoring the deleted save failed");
        }
    }
    sigil_ps2_save_free(&rez);
    sigil_ps2_save_free(&system);
    sigil_card_listing_free(before);
    sigil_card_listing_free(after);
    sigil_card_listing_free(restored);
    sigil_ps2_card_free(&card);
}

/* A save that doesn't fit is refused before anything changes. */
static void check_full_card_refuses(const corpus_table *manifest) {
    sigil_ps2_card card;
    if (!load_card(manifest, "mymc-mc01", &card)) return;
    sigil_card_listing *listing = NULL;
    sigil_ps2_save save;
    memset(&save, 0, sizeof(save));
    if (sigil_ps2_card_list(&card, &listing) == SIGIL_OK && listing->entry_count > 0 &&
        sigil_ps2_extract(&card, listing->entries[listing->entry_count - 1].first_block, &save) == SIGIL_OK &&
        save.file_count > 0) {
        uint32_t huge = (card.alloc_end + 1) * card.cluster_size;
        uint8_t *big = (uint8_t *)calloc(huge, 1);
        uint8_t *old = save.files[0].data;
        uint8_t old_len[4];
        memcpy(old_len, save.files[0].entry + 4, 4);
        save.files[0].data = big;
        save.files[0].entry[4] = (uint8_t)huge;
        save.files[0].entry[5] = (uint8_t)(huge >> 8);
        save.files[0].entry[6] = (uint8_t)(huge >> 16);
        save.files[0].entry[7] = (uint8_t)(huge >> 24);
        save.entry[0x40] = 'X';
        size_t size = sigil_ps2_card_file_size(&card);
        uint8_t *before = (uint8_t *)malloc(size), *after = (uint8_t *)malloc(size);
        if (big && before && after && sigil_ps2_card_write(&card, before) == SIGIL_OK) {
            if (sigil_ps2_inject(&card, &save) != SIGIL_ERR_NO_SPACE) fail("mymc-mc01", "an oversized save went in");
            if (sigil_ps2_card_write(&card, after) != SIGIL_OK || memcmp(before, after, size) != 0) {
                fail("mymc-mc01", "a refused inject changed the card");
            }
        }
        free(before);
        free(after);
        save.files[0].data = old;
        memcpy(save.files[0].entry + 4, old_len, 4);
        free(big);
    }
    sigil_ps2_save_free(&save);
    sigil_card_listing_free(listing);
    sigil_ps2_card_free(&card);
}

/* Reads the files of one folder sample into folder-file form, paths relative
 * to the save folder. `*folder` receives the folder's name. */
static size_t load_folder(const corpus_table *manifest, const char *id, sigil_ps2_folder_file *files, size_t cap,
                          char folder[64]) {
    size_t n = 0;
    folder[0] = '\0';
    for (size_t r = 0; r < manifest->nrows && n < cap; r++) {
        if (strcmp(corpus_get(manifest, r, "id"), id) != 0) continue;
        const char *path = corpus_get(manifest, r, "path");
        const char *slash = strchr(path, '/');
        if (!slash) continue;
        snprintf(folder, 64, "%.*s", (int)(slash - path), path);
        char full[1024];
        if (corpus_sample_path("ps2", id, path, full, sizeof(full)) != 0) continue;
        size_t len = 0;
        uint8_t *data = corpus_read_file(full, &len);
        if (!data) continue;
        snprintf(files[n].path, sizeof(files[n].path), "%s", slash + 1);
        files[n].data = data;
        files[n].len = len;
        n++;
    }
    return n;
}

static void free_folder(sigil_ps2_folder_file *files, size_t n) {
    for (size_t i = 0; i < n; i++) free(files[i].data);
}

typedef struct {
    const char *id;
    const char *files[4];      /* in index order */
    const char *checked;       /* one file whose times are pinned below */
    long long   created;
    long long   modified;
} folder_fact;

/* Read from each sample's _pcsx2_index: AetherSX2 block style and ARMSX2 flow style. */
static const folder_fact FOLDER_FACTS[] = {
    { "ace-combat-04-aethersx2", { "icon.sys", "icon1.ico", "BASLUS-20152AC04", NULL },
      "BASLUS-20152AC04", 1784833828LL, 1784833829LL },
    { "athf-aethersx2", { "BASLUS-21633-ATHF-1", "game.ico", "icon.sys", NULL },
      "icon.sys", 1778155573LL, 1778155574LL },
    { "7-wonders-armsx2", { "icon.sys", "list.ico", "BASLUS-21693", NULL },
      "list.ico", 1784865698LL, 1784865699LL },
};

static long long entry_time(const uint8_t *tod) {
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_sec = tod[1];
    t.tm_min = tod[2];
    t.tm_hour = tod[3];
    t.tm_mday = tod[4];
    t.tm_mon = tod[5] - 1;
    t.tm_year = (tod[6] | (tod[7] << 8)) - 1900;
    return (long long)timegm(&t);
}

static const sigil_ps2_folder_file *find_path(const sigil_ps2_folder_file *files, size_t n, const char *path) {
    for (size_t i = 0; i < n; i++) {
        if (strcmp(files[i].path, path) == 0) return &files[i];
    }
    return NULL;
}

/* Packing a folder follows its index; the packed save goes through a card
 * and back out to the same files. */
static void check_folder(const corpus_table *manifest, const folder_fact *fact) {
    sigil_ps2_folder_file files[16];
    char folder[64];
    size_t n = load_folder(manifest, fact->id, files, 16, folder);
    if (n == 0) return;
    g_cards++;

    sigil_ps2_save save, back;
    memset(&save, 0, sizeof(save));
    memset(&back, 0, sizeof(back));
    sigil_ps2_card card;
    memset(&card, 0, sizeof(card));
    sigil_ps2_folder_file *unpacked = NULL;
    size_t unpacked_n = 0;
    if (sigil_ps2_pack(folder, files, n, &save) != SIGIL_OK) {
        fail(fact->id, "pack failed");
    } else {
        for (size_t i = 0; i < 4 && fact->files[i]; i++) {
            char name[33];
            memcpy(name, i < save.file_count ? (const char *)save.files[i].entry + 0x40 : "", 32);
            name[32] = '\0';
            if (i >= save.file_count || strcmp(name, fact->files[i]) != 0) fail(fact->id, "files out of index order");
            if (i < save.file_count && strcmp(name, fact->checked) == 0 &&
                (entry_time(save.files[i].entry + 0x08) != fact->created ||
                 entry_time(save.files[i].entry + 0x18) != fact->modified)) {
                fail(fact->id, "file times differ from the index");
            }
        }
        sigil_card_listing *listing = NULL;
        if (sigil_ps2_card_format(&card, MC02_TOD) != SIGIL_OK || sigil_ps2_inject(&card, &save) != SIGIL_OK ||
            sigil_ps2_card_list(&card, &listing) != SIGIL_OK || listing->entry_count != 1 ||
            sigil_ps2_extract(&card, listing->entries[0].first_block, &back) != SIGIL_OK ||
            sigil_ps2_unpack(&back, &unpacked, &unpacked_n) != SIGIL_OK) {
            fail(fact->id, "card round trip failed");
        } else {
            for (size_t i = 0; i < n; i++) {
                if (strncmp(files[i].path, "_pcsx2_", 7) == 0) continue;
                const sigil_ps2_folder_file *u = find_path(unpacked, unpacked_n, files[i].path);
                if (!u || u->len != files[i].len || memcmp(u->data, files[i].data, u->len) != 0) {
                    fail(fact->id, "a file changed on its way through a card");
                }
            }
            sigil_ps2_save again;
            if (sigil_ps2_pack(folder, unpacked, unpacked_n, &again) != SIGIL_OK) {
                fail(fact->id, "the unpacked folder does not pack");
            } else {
                bool same = again.file_count == save.file_count && memcmp(again.entry, save.entry, 512) == 0;
                for (size_t i = 0; same && i < save.file_count; i++) {
                    same = memcmp(again.files[i].entry, save.files[i].entry, 512) == 0;
                }
                if (!same) fail(fact->id, "unpack then pack changes the entries");
                sigil_ps2_save_free(&again);
            }
        }
        sigil_card_listing_free(listing);
    }
    sigil_ps2_folder_files_free(unpacked, unpacked_n);
    sigil_ps2_save_free(&save);
    sigil_ps2_save_free(&back);
    if (card.clusters) sigil_ps2_card_free(&card);
    free_folder(files, n);
}

/* A card save whose folder PCSX2 couldn't rebuild from the index alone, like
 * the hidden BEDATA-SYSTEM folder, carries its entry in _pcsx2_meta_directory. */
static void check_meta_for_hidden_folder(const corpus_table *manifest) {
    sigil_ps2_card card;
    if (!load_card(manifest, "mymc-mc01", &card)) return;
    sigil_card_listing *listing = NULL;
    sigil_ps2_save save, again;
    memset(&save, 0, sizeof(save));
    memset(&again, 0, sizeof(again));
    sigil_ps2_folder_file *files = NULL;
    size_t n = 0;
    const sigil_card_entry *e = NULL;
    if (sigil_ps2_card_list(&card, &listing) == SIGIL_OK) e = find_entry(listing, "BEDATA-SYSTEM");
    if (!e || sigil_ps2_extract(&card, e->first_block, &save) != SIGIL_OK ||
        sigil_ps2_unpack(&save, &files, &n) != SIGIL_OK) {
        fail("mymc-mc01", "BEDATA-SYSTEM did not unpack");
    } else {
        if (!find_path(files, n, "_pcsx2_meta_directory")) fail("mymc-mc01", "hidden folder lost its mode");
        if (sigil_ps2_pack("BEDATA-SYSTEM", files, n, &again) != SIGIL_OK ||
            memcmp(again.entry, save.entry, 4) != 0) {
            fail("mymc-mc01", "hidden folder's mode did not survive a folder round trip");
        }
    }
    sigil_ps2_folder_files_free(files, n);
    sigil_ps2_save_free(&save);
    sigil_ps2_save_free(&again);
    sigil_card_listing_free(listing);
    sigil_ps2_card_free(&card);
}

int main(void) {
    char path[1024];
    corpus_table manifest, entries;
    if (corpus_platform_path("ps2", "manifest.tsv", path, sizeof(path)) != 0 ||
        corpus_load(path, &manifest) != 0) {
        fprintf(stderr, "SKIP: no ps2 manifest\n");
        return TEST_SKIP;
    }
    if (corpus_platform_path("ps2", "entries.tsv", path, sizeof(path)) != 0 ||
        corpus_load(path, &entries) != 0) {
        corpus_free(&manifest);
        fprintf(stderr, "SKIP: no ps2 entries\n");
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
    check_format_matches_mymc(&manifest);
    check_folder_superblock(&manifest);
    check_write_reproduces_file(&manifest);
    check_rebuild(&manifest, &entries);
    check_delete_and_restore(&manifest);
    check_full_card_refuses(&manifest);
    for (size_t i = 0; i < sizeof(FOLDER_FACTS) / sizeof(FOLDER_FACTS[0]); i++) check_folder(&manifest, &FOLDER_FACTS[i]);
    check_meta_for_hidden_folder(&manifest);
    corpus_free(&manifest);
    corpus_free(&entries);

    printf("ps2 cards: %d checked, %d failures\n", g_cards, g_fails);
    if (g_fails) return 1;
    return g_cards ? 0 : TEST_SKIP;
}
