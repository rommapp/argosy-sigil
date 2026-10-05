// SPDX-License-Identifier: MPL-2.0
/* RetroArch's RZIP: both codecs over several chunks decode to the bytes they
 * were made from, a damaged file doesn't, and a compressed save collects and
 * hashes as the same save uncompressed. */
#include "legacy_units.h"
#include "rzip.h"
#include <stdio.h>
#include <zlib.h>
#include <zstd.h>

static int g_fails = 0;

static void fail(const char *where, const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", where, what);
    g_fails++;
}

#define CHUNK 4096u

/* An RZIP file of `data` as libretro-common writes one, `version` 1 (deflate) or 2 (Zstandard). */
static uint8_t *rzip_of(const uint8_t *data, size_t len, int version, size_t *out_len) {
    size_t cap = SIGIL_RZIP_HEADER_SIZE + (len / CHUNK + 1) * (4 + compressBound(CHUNK) + ZSTD_compressBound(CHUNK));
    uint8_t *out = (uint8_t *)malloc(cap);
    memcpy(out, "#RZIPv", 6);
    out[6] = (uint8_t)version;
    out[7] = '#';
    sigil_write_le32(out + 8, CHUNK);
    sigil_write_le64(out + 12, len);
    size_t at = SIGIL_RZIP_HEADER_SIZE;
    for (size_t off = 0; off < len; off += CHUNK) {
        size_t n = len - off < CHUNK ? len - off : CHUNK;
        size_t made = cap - at - 4;
        if (version == 1) {
            uLongf z = (uLongf)made;
            compress2(out + at + 4, &z, data + off, (uLong)n, 6);
            made = z;
        } else {
            made = ZSTD_compress(out + at + 4, made, data + off, n, 3);
        }
        sigil_write_le32(out + at, (uint32_t)made);
        at += 4 + made;
    }
    *out_len = at;
    return out;
}

static uint8_t g_data[10000];

static void check_decode(void) {
    for (size_t i = 0; i < sizeof(g_data); i++) g_data[i] = (uint8_t)(i * 31 + i / 97);
    for (int version = 1; version <= 2; version++) {
        const char *where = version == 1 ? "deflate" : "zstd";
        size_t len = 0, plain_len = 0;
        uint8_t *rz = rzip_of(g_data, sizeof(g_data), version, &len), *plain = NULL;
        if (sigil_rzip_decode(rz, len, 1u << 20, &plain, &plain_len) != SIGIL_OK || plain_len != sizeof(g_data) ||
            memcmp(plain, g_data, plain_len) != 0) {
            fail(where, "doesn't decode to the bytes it was made from");
        }
        free(plain);
        plain = NULL;
        if (sigil_rzip_decode(rz, len - 10, 1u << 20, &plain, &plain_len) == SIGIL_OK) fail(where, "a cut file decodes");
        sigil_write_le64(rz + 12, sizeof(g_data) + 1);
        if (sigil_rzip_decode(rz, len, 1u << 20, &plain, &plain_len) == SIGIL_OK) fail(where, "a wrong total decodes");
        sigil_write_le64(rz + 12, sizeof(g_data));
        rz[SIGIL_RZIP_HEADER_SIZE + 6] ^= 0xFF;
        if (sigil_rzip_decode(rz, len, 1u << 20, &plain, &plain_len) == SIGIL_OK) fail(where, "a damaged chunk decodes");
        free(rz);
    }
    if (sigil_rzip_is(g_data, sizeof(g_data))) fail("plain", "uncompressed bytes read as RZIP");
}

typedef struct {
    sigil_sync_request req;
    sigil_result       result;
    const char        *ids[1];
} game;

static void make_game(game *g, mem_root *root) {
    memset(g, 0, sizeof(*g));
    g->result.struct_version = SIGIL_RESULT_V4;
    g->result.platform = SIGIL_PLATFORM_GB;
    g->ids[0] = "LA";
    g->req.struct_version = SIGIL_SYNC_REQUEST_V1;
    g->req.save.struct_version = SIGIL_SAVE_REQUEST_V1;
    g->req.save.layout = "gambatte";
    g->req.save.platform = "gb";
    g->req.save.content_path = "Zelda.gb";
    g->req.save.result = &g->result;
    g->req.save.listing = root->listing;
    g->req.save.listing_count = root->count;
    g->req.save.open = root_open;
    g->req.save.open_ctx = root;
    g->req.game_ids = g->ids;
    g->req.game_id_count = 1;
    g->req.write = root_write;
    g->req.write_ctx = root;
}

/* A save RetroArch compressed collects, and hashes, as the same save uncompressed. */
static void check_save(void) {
    uint8_t ram[8192];
    for (size_t i = 0; i < sizeof(ram); i++) ram[i] = (uint8_t)(i ^ (i >> 5));
    size_t rz_len = 0;
    uint8_t *rz = rzip_of(ram, sizeof(ram), 2, &rz_len);
    mem_root plain = {0}, packed = {0};
    root_put(&plain, "Zelda.srm", ram, sizeof(ram));
    root_put(&packed, "Zelda.srm", rz, rz_len);
    game a, b;
    make_game(&a, &plain);
    make_game(&b, &packed);
    sigil_sync_result *ra = NULL, *rb = NULL;
    if (sigil_collect(&a.req, &ra) != SIGIL_OK || sigil_collect(&b.req, &rb) != SIGIL_OK || !rb->data ||
        rb->len != sizeof(ram) || strcmp(ra->identity_hash, rb->identity_hash) != 0) {
        fail("rzip save", "collect doesn't give the uncompressed save");
    }
    sigil_sync_result_free(ra);
    sigil_sync_result_free(rb);

    sigil_save_unit *ua = NULL, *ub = NULL;
    if (sigil_save_resolve(&a.req.save, &ua) != SIGIL_OK || sigil_save_resolve(&b.req.save, &ub) != SIGIL_OK ||
        sigil_save_hash(ua, root_open, &plain) != SIGIL_OK || sigil_save_hash(ub, root_open, &packed) != SIGIL_OK ||
        strcmp(ua->content_hash, ub->content_hash) != 0) {
        fail("rzip save", "hash doesn't cover the uncompressed save");
    }
    sigil_save_unit_free(ua);
    sigil_save_unit_free(ub);
    root_free(&plain);
    root_free(&packed);
    free(rz);
}

/* A raw PS1 card holding one save of `blocks` blocks named `name`, as FileAccessTest builds one. */
static void ps1_card(uint8_t card[128 * 1024], const char *name, int blocks) {
    memset(card, 0, 128 * 1024);
    card[0] = 'M';
    card[1] = 'C';
    for (int frame = 1; frame < 16; frame++) {
        card[frame * 128] = 0xA0;
        card[frame * 128 + 8] = 0xFF;
        card[frame * 128 + 9] = 0xFF;
    }
    for (int i = 0; i < blocks; i++) {
        uint8_t *f = card + (1 + i) * 128;
        uint32_t state = i == 0 ? 0x51 : i == blocks - 1 ? 0x53 : 0x52;
        uint32_t link = i == blocks - 1 ? 0xFFFF : (uint32_t)(1 + i);
        sigil_write_le32(f, state);
        sigil_write_le32(f + 4, i == 0 ? (uint32_t)blocks * 8192 : 0);
        f[8] = (uint8_t)link;
        f[9] = (uint8_t)(link >> 8);
        if (i == 0) memcpy(f + 10, name, strlen(name));
    }
    for (int i = 0; i < blocks * 8192; i++) card[8192 + i] = (uint8_t)(i * 13);
}

/* A compressed card collects as the card does, on the card path rather than the file path. */
static void check_card(void) {
    static uint8_t card[128 * 1024];
    ps1_card(card, "BASLUSP01041CROSS", 2);
    size_t rz_len = 0;
    uint8_t *rz = rzip_of(card, sizeof(card), 1, &rz_len);
    mem_root plain = {0}, packed = {0};
    root_put(&plain, "Chrono Cross.srm", card, sizeof(card));
    root_put(&packed, "Chrono Cross.srm", rz, rz_len);
    sigil_sync_result *r[2] = { NULL, NULL };
    mem_root *roots[2] = { &plain, &packed };
    for (int i = 0; i < 2; i++) {
        game g;
        make_game(&g, roots[i]);
        g.result.platform = SIGIL_PLATFORM_PSX;
        snprintf(g.result.title_id, sizeof(g.result.title_id), "SLUS-01041");
        g.ids[0] = "SLUS-01041";
        g.req.save.layout = "pcsx_rearmed";
        g.req.save.platform = "psx";
        g.req.save.content_path = "Chrono Cross.cue";
        sigil_collect(&g.req, &r[i]);
    }
    if (!r[0] || !r[1] || !r[1]->data || strcmp(r[0]->identity_hash, r[1]->identity_hash) != 0) {
        fail("rzip card", "a compressed card doesn't collect as the card");
    }
    sigil_sync_result_free(r[0]);
    sigil_sync_result_free(r[1]);
    root_free(&plain);
    root_free(&packed);
    free(rz);
}

int main(void) {
    check_decode();
    check_save();
    check_card();
    printf("rzip: %d failures\n", g_fails);
    return g_fails ? 1 : 0;
}
