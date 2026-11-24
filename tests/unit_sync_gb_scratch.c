// SPDX-License-Identifier: MPL-2.0
/* The identity of a Pokemon cart's RAM reads its scratch ranges as zeros:
 * a write there alone keeps the identity, a save changes it, and a RAM
 * whose layout doesn't check out hashes whole. Offsets and checksums are
 * pret's and PKHeX's (src/scratch_gb.c). */
#include "legacy_units.h"
#include <stdio.h>

#define RAM_LEN 0x8000u

static int g_fails = 0;

static void fail(const char *where, const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", where, what);
    g_fails++;
}

typedef struct {
    const char *name;
    uint32_t    feature;
    size_t      data_first, data_last, checksum_at;
    bool        sum8_complement;
} game_layout;

static const game_layout GEN1 = { "gen1", SIGIL_FEATURE_GB_SCRATCH_GEN1, 0x2598, 0x3522, 0x3523, true };
static const game_layout GOLD = { "gold", SIGIL_FEATURE_GB_SCRATCH_GOLD_SILVER, 0x2009, 0x2D68, 0x2D69, false };
static const game_layout CRYSTAL = { "crystal", SIGIL_FEATURE_GB_SCRATCH_CRYSTAL, 0x2009, 0x2B82, 0x2D0D, false };

static void seal(const game_layout *g, uint8_t *ram) {
    uint32_t sum = 0;
    for (size_t i = g->data_first; i <= g->data_last; i++) sum += ram[i];
    if (g->sum8_complement) {
        ram[g->checksum_at] = (uint8_t)~sum;
    } else {
        ram[g->checksum_at] = (uint8_t)sum;
        ram[g->checksum_at + 1] = (uint8_t)(sum >> 8);
    }
}

static void saved_ram(const game_layout *g, uint8_t *ram) {
    for (size_t i = 0; i < RAM_LEN; i++) ram[i] = (uint8_t)(i * 31 + i / 97);
    seal(g, ram);
}

static void identity(uint32_t features, const uint8_t *unit, size_t len, char out[33]) {
    if (sigil_unit_identity("gbc", features, unit, len, out) != SIGIL_OK) fail("identity", "refused");
}

/* The md5 of `ram` with each [first, last] range zeroed, built here rather than by sigil. */
static void masked_md5(const uint8_t *ram, const size_t ranges[][2], size_t count, char out[33]) {
    static uint8_t copy[RAM_LEN];
    memcpy(copy, ram, RAM_LEN);
    for (size_t r = 0; r < count; r++) memset(copy + ranges[r][0], 0, ranges[r][1] - ranges[r][0] + 1);
    sigil_md5_of(copy, RAM_LEN, out);
}

/* Changing `offset` keeps the identity when `same`, and changes it otherwise. */
static void check_offset(const game_layout *g, size_t offset, bool same, const char *label) {
    static uint8_t ram[RAM_LEN];
    saved_ram(g, ram);
    char before[33], after[33];
    identity(g->feature, ram, RAM_LEN, before);
    ram[offset] ^= 0x5A;
    if (offset >= g->data_first && offset <= g->data_last) seal(g, ram);
    identity(g->feature, ram, RAM_LEN, after);
    if ((strcmp(before, after) == 0) != same) fail(label, same ? "a scratch write changed the identity" : "a change kept the identity");
}

static void check_masks(void) {
    static uint8_t ram[RAM_LEN];
    char got[33], want[33];

    const size_t gen1[][2] = { { 0x0000, 0x0497 } };
    saved_ram(&GEN1, ram);
    identity(GEN1.feature, ram, RAM_LEN, got);
    masked_md5(ram, gen1, 1, want);
    if (strcmp(got, want) != 0) fail("gen1", "identity isn't the RAM with the sprite buffers zeroed");

    const size_t gold[][2] = { { 0x0000, 0x05FF }, { 0x1800, 0x1FFF } };
    saved_ram(&GOLD, ram);
    identity(GOLD.feature, ram, RAM_LEN, got);
    masked_md5(ram, gold, 2, want);
    if (strcmp(got, want) != 0) fail("gold", "identity isn't the RAM with scratch and the window stack zeroed");

    const size_t crystal[][2] = { { 0x0000, 0x05FF } };
    saved_ram(&CRYSTAL, ram);
    identity(CRYSTAL.feature, ram, RAM_LEN, got);
    masked_md5(ram, crystal, 1, want);
    if (strcmp(got, want) != 0) fail("crystal", "identity isn't the RAM with scratch zeroed");
}

static void check_offsets(void) {
    check_offset(&GEN1, 0x0000, true, "gen1 first sprite byte");
    check_offset(&GEN1, 0x0497, true, "gen1 last sprite byte");
    check_offset(&GEN1, 0x0498, false, "gen1 past the sprite buffers");
    check_offset(&GEN1, 0x0598, false, "gen1 hall of fame");
    check_offset(&GEN1, 0x2600, false, "gen1 save");
    check_offset(&GEN1, 0x4000, false, "gen1 box");

    check_offset(&GOLD, 0x05FF, true, "gold last scratch byte");
    check_offset(&GOLD, 0x0600, false, "gold mail");
    check_offset(&GOLD, 0x17FF, false, "gold before the window stack");
    check_offset(&GOLD, 0x1800, true, "gold window stack bottom");
    check_offset(&GOLD, 0x1FFF, true, "gold window stack top");
    check_offset(&GOLD, 0x2100, false, "gold save");

    check_offset(&CRYSTAL, 0x05FF, true, "crystal last scratch byte");
    check_offset(&CRYSTAL, 0x1800, false, "crystal bank 0 past scratch");
    check_offset(&CRYSTAL, 0x2100, false, "crystal save");
}

/* Without its layout's bit, its checksum or its size, the RAM hashes whole. */
static void check_fallbacks(void) {
    static uint8_t ram[RAM_LEN + 512];
    char got[33], want[33];

    saved_ram(&GEN1, ram);
    sigil_md5_of(ram, RAM_LEN, want);
    identity(0, ram, RAM_LEN, got);
    if (strcmp(got, want) != 0) fail("no layout bit", "masked");
    identity(GOLD.feature, ram, RAM_LEN, got);
    if (strcmp(got, want) != 0) fail("another game's layout", "masked");

    ram[GEN1.checksum_at] ^= 0x01;
    sigil_md5_of(ram, RAM_LEN, want);
    identity(GEN1.feature, ram, RAM_LEN, got);
    if (strcmp(got, want) != 0) fail("checksum fails", "masked");

    saved_ram(&GEN1, ram);
    memset(ram + RAM_LEN, 0x11, 512);
    sigil_md5_of(ram, RAM_LEN + 512, want);
    identity(GEN1.feature, ram, RAM_LEN + 512, got);
    if (strcmp(got, want) != 0) fail("larger RAM", "masked");

    if (sigil_unit_identity("nes", 0, ram, RAM_LEN, got) != SIGIL_ERR_UNSUPPORTED_FORMAT) fail("nes", "answered");
}

/* A clock cart's zip has the identity of its RAM alone. */
static void check_clock_zip(void) {
    static uint8_t ram[RAM_LEN];
    saved_ram(&GOLD, ram);
    uint8_t clock[48] = { 0 };
    const char *names[] = { "save.sram", "clock.rtc" };
    uint8_t *data[] = { ram, clock };
    size_t lens[] = { RAM_LEN, sizeof(clock) };
    size_t zip_len = 0;
    uint8_t *zip = legacy_zip(names, data, lens, 2, &zip_len);
    char raw[33], zipped[33];
    identity(GOLD.feature | SIGIL_FEATURE_RTC, ram, RAM_LEN, raw);
    identity(GOLD.feature | SIGIL_FEATURE_RTC, zip, zip_len, zipped);
    if (!zip || strcmp(raw, zipped) != 0) fail("gold zip", "identity differs from the raw RAM's");
    free(zip);
}

typedef struct {
    sigil_sync_request req;
    sigil_result       result;
    const char        *ids[1];
} game;

static void make_game(game *g, mem_root *root, uint32_t features) {
    memset(g, 0, sizeof(*g));
    g->result.struct_version = SIGIL_RESULT_V4;
    g->result.platform = SIGIL_PLATFORM_GB;
    g->result.features = features;
    g->ids[0] = "Yellow";
    g->req.struct_version = SIGIL_SYNC_REQUEST_V1;
    g->req.save.struct_version = SIGIL_SAVE_REQUEST_V1;
    g->req.save.layout = "gambatte";
    g->req.save.platform = "gb";
    g->req.save.content_path = "Yellow.gb";
    g->req.save.result = &g->result;
    g->req.save.listing = root->listing;
    g->req.save.listing_count = root->count;
    g->req.save.open = root_open;
    g->req.save.open_ctx = root;
    g->req.game_ids = g->ids;
    g->req.game_id_count = 1;
    g->req.write = root_write;
    g->req.remove = root_remove;
    g->req.write_ctx = root;
}

/* Collect reports the masked identity and the RAM's own content hash, and a
 * scratch write on disk alone doesn't block a restore. */
static void check_sync(void) {
    static uint8_t ram[RAM_LEN];
    saved_ram(&GEN1, ram);
    mem_root root = {0};
    root_put(&root, "Yellow.srm", ram, RAM_LEN);
    game g;
    make_game(&g, &root, GEN1.feature);
    sigil_sync_result *r = NULL;
    char content[33], ident[33];
    sigil_md5_of(ram, RAM_LEN, content);
    identity(GEN1.feature, ram, RAM_LEN, ident);
    if (sigil_collect(&g.req, &r) != SIGIL_OK || !r) {
        fail("collect", "refused");
        root_free(&root);
        sigil_sync_result_free(r);
        return;
    }
    if (strcmp(r->content_hash, content) != 0) fail("collect", "content hash isn't the RAM's md5");
    if (strcmp(r->identity_hash, ident) != 0) fail("collect", "identity isn't the masked one");

    root_find(&root, "Yellow.srm")->data[0x0100] ^= 0x5A;
    make_game(&g, &root, GEN1.feature);
    g.req.state = r->state;
    g.req.state_len = r->state_len;
    sigil_sync_result *w = NULL;
    if (sigil_restore(&g.req, ram, RAM_LEN, &w) != SIGIL_OK) fail("restore", "a scratch write blocked it");
    sigil_sync_result_free(w);

    root_find(&root, "Yellow.srm")->data[0x2600] ^= 0x5A;
    make_game(&g, &root, GEN1.feature);
    g.req.state = r->state;
    g.req.state_len = r->state_len;
    w = NULL;
    if (sigil_restore(&g.req, ram, RAM_LEN, &w) != SIGIL_ERR_CONFLICT) fail("restore", "a changed save didn't block it");
    sigil_sync_result_free(w);
    sigil_sync_result_free(r);
    root_free(&root);
}

int main(void) {
    check_masks();
    check_offsets();
    check_fallbacks();
    check_clock_zip();
    check_sync();
    if (g_fails) {
        fprintf(stderr, "%d failure(s)\n", g_fails);
        return 1;
    }
    printf("unit_sync_gb_scratch: ok\n");
    return 0;
}
