// SPDX-License-Identifier: MPL-2.0
/* A Switch package read without keys gives the game its tickets name.
 * Packages are built here: PFS0 for NSP and NSZ, an HFS0 secure partition
 * under the root HFS0 at 0xF000 for XCI and XCZ. A ticket is laid out as
 * switchbrew's "Ticket" page gives it: a little-endian signature type, the
 * signature and its padding, then the ticket data with the issuer at 0x0, the
 * title key block at 0x40 and the rights id at 0x160. */
#include "sigil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMAGE_MAX  0x20000u
#define NCA_SIZE   0xC00u
#define XCI_ROOT   0xF000u
#define MAX_GUARDS 32

#define SIG_RSA4096 0x23Cu /* 0x200 signature, 0x3C padding */
#define SIG_RSA2048 0x13Cu /* 0x100 signature, 0x3C padding */
#define SIG_ECDSA   0x7Cu  /* 0x3C signature, 0x40 padding */

static void put_le32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static void put_le64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

/* Reads anywhere in a ticket but its signature type, issuer and rights id
 * are recorded: the reader must never touch the signature, the title key
 * block, the ticket and device ids or the account id. */
typedef struct { uint64_t lo, hi; } span;

typedef struct {
    const uint8_t *buf;
    size_t len;
    span guards[MAX_GUARDS];
    int guard_count;
    int touched;
} mem_ctx;

static int mem_read(void *ctx, uint64_t off, void *buf, size_t len) {
    mem_ctx *m = (mem_ctx *)ctx;
    for (int i = 0; i < m->guard_count; i++) {
        if (off < m->guards[i].hi && off + len > m->guards[i].lo) m->touched = 1;
    }
    if (off >= m->len) return 0;
    size_t avail = m->len - (size_t)off;
    size_t n = len < avail ? len : avail;
    memcpy(buf, m->buf + off, n);
    return (int)n;
}

static int64_t mem_size(void *ctx) { return (int64_t)((mem_ctx *)ctx)->len; }

typedef struct {
    uint8_t bytes[4 + SIG_RSA4096 + 0x2C0];
    size_t len;
    size_t sig;
} ticket;

/* A ticket for `rights_id` (32 hex digits). */
static ticket make_ticket(uint32_t type, size_t sig, const char *rights_id) {
    ticket t;
    memset(&t, 0, sizeof t);
    put_le32(t.bytes, type);
    memset(t.bytes + 4, 0x11, sig);
    uint8_t *d = t.bytes + 4 + sig;
    memcpy(d, "Root-CA00000003-XS00000020", 26);
    memset(d + 0x40, 0xAA, 0x100);  /* title key block */
    memset(d + 0x150, 0xBB, 0x10);  /* ticket id, device id */
    for (int i = 0; i < 16; i++) {
        unsigned b;
        sscanf(rights_id + 2 * i, "%2x", &b);
        d[0x160 + i] = (uint8_t)b;
    }
    memset(d + 0x170, 0xCC, 4);     /* account id */
    t.len = 4 + sig + 0x2C0;
    t.sig = sig;
    return t;
}

static ticket rsa2048(const char *rights_id) {
    return make_ticket(0x10004, SIG_RSA2048, rights_id);
}

typedef struct {
    const char *name;
    const uint8_t *data;
    size_t len;
    size_t sig; /* a ticket's signature and padding, to guard; 0 for any other file */
} member;

static member tik(const char *name, const ticket *t) {
    member m = { name, t->bytes, t->len, t->sig };
    return m;
}

static uint8_t junk_nca[NCA_SIZE];
static uint8_t plain_nca[NCA_SIZE];

static member nca(const char *name) {
    member m = { name, junk_nca, sizeof junk_nca, 0 };
    return m;
}

static uint8_t g_img[IMAGE_MAX];
static mem_ctx g_ctx;

/* A PFS0 (24-byte entries) or HFS0 (64-byte entries) partition at `at`.
 * `where` gets each member's offset in `img`. Returns the partition's size. */
static size_t put_partition(uint8_t *img, size_t at, const char *magic, size_t entry_size,
                            const member *m, size_t n, size_t *where) {
    size_t strtab = 0;
    for (size_t i = 0; i < n; i++) strtab += strlen(m[i].name) + 1;
    size_t data = at + 16 + n * entry_size + strtab;

    memcpy(img + at, magic, 4);
    put_le32(img + at + 4, (uint32_t)n);
    put_le32(img + at + 8, (uint32_t)strtab);
    size_t name_off = 0, file_off = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t *e = img + at + 16 + i * entry_size;
        put_le64(e, file_off);
        put_le64(e + 8, m[i].len);
        put_le32(e + 16, (uint32_t)name_off);
        memcpy(img + at + 16 + n * entry_size + name_off, m[i].name, strlen(m[i].name) + 1);
        memcpy(img + data + file_off, m[i].data, m[i].len);
        if (where) where[i] = data + file_off;
        name_off += strlen(m[i].name) + 1;
        file_off += m[i].len;
    }
    return data + file_off - at;
}

static void guard(const member *m, size_t n, const size_t *where, size_t base) {
    for (size_t i = 0; i < n; i++) {
        if (!m[i].sig) continue;
        uint64_t t = base + where[i], d = t + 4 + m[i].sig;
        span s[3] = { { t + 4, d }, { d + 0x40, d + 0x160 }, { d + 0x170, t + m[i].len } };
        for (int k = 0; k < 3; k++) g_ctx.guards[g_ctx.guard_count++] = s[k];
    }
}

static void build_nsp(const member *m, size_t n) {
    memset(g_img, 0, sizeof g_img);
    memset(&g_ctx, 0, sizeof g_ctx);
    size_t where[16];
    g_ctx.len = put_partition(g_img, 0, "PFS0", 24, m, n, where);
    g_ctx.buf = g_img;
    guard(m, n, where, 0);
}

static void build_xci(const member *m, size_t n) {
    static uint8_t secure[IMAGE_MAX / 2];
    memset(g_img, 0, sizeof g_img);
    memset(secure, 0, sizeof secure);
    memset(&g_ctx, 0, sizeof g_ctx);
    size_t where[16], root_where[1];
    size_t secure_len = put_partition(secure, 0, "HFS0", 64, m, n, where);
    member root = { "secure", secure, secure_len, 0 };
    g_ctx.len = XCI_ROOT + put_partition(g_img, XCI_ROOT, "HFS0", 64, &root, 1, root_where);
    g_ctx.buf = g_img;
    guard(m, n, where, root_where[0]);
}

static int failures = 0;

static void expect(const char *label, const char *name, const sigil_options *opts,
                   int want_rc, const char *want_title, const char *want_raw);

/* The same members in an NSP and in a card's secure partition. */
static const struct {
    const char *kind;
    const char *file;
    void (*build)(const member *m, size_t n);
} containers[] = {
    { "nsp", "game.nsp", build_nsp },
    { "xci", "game.xci", build_xci },
};

/* Members that give no id, in either container. */
static void refused(const char *what, const sigil_options *opts, const member *m, size_t n) {
    for (size_t c = 0; c < sizeof containers / sizeof containers[0]; c++) {
        char label[96];
        snprintf(label, sizeof label, "%s, %s", containers[c].kind, what);
        containers[c].build(m, n);
        expect(label, containers[c].file, opts, SIGIL_ERR_NEEDS_KEY, NULL, NULL);
    }
}

static void expect(const char *label, const char *name, const sigil_options *opts,
                   int want_rc, const char *want_title, const char *want_raw) {
    sigil_io io = { mem_read, mem_size, NULL, &g_ctx };
    sigil_result r;
    g_ctx.touched = 0;
    int rc = sigil_extract_from_io(&io, name, SIGIL_PLATFORM_SWITCH, opts, &r);
    if (rc != want_rc) {
        fprintf(stderr, "FAIL %s: rc=%d want %d\n", label, rc, want_rc);
        failures++;
        return;
    }
    if (g_ctx.touched) {
        fprintf(stderr, "FAIL %s: read a ticket's signature, title key, device or account id\n", label);
        failures++;
    }
    if (want_rc != SIGIL_OK) return;
    if (strcmp(r.title_id, want_title) != 0 || strcmp(r.raw_serial, want_raw) != 0
        || strcmp(r.save_id, want_title) != 0) {
        fprintf(stderr, "FAIL %s: title_id=%s raw_serial=%s save_id=%s, want %s %s\n",
                label, r.title_id, r.raw_serial, r.save_id, want_title, want_raw);
        failures++;
    }
    if (r.source != SIGIL_SOURCE_BINARY || r.platform != SIGIL_PLATFORM_SWITCH
        || r.usage != SIGIL_USAGE_FOLDER_EXACT) {
        fprintf(stderr, "FAIL %s: source=%d platform=%d usage=%d\n",
                label, (int)r.source, (int)r.platform, (int)r.usage);
        failures++;
    }
    if (r.switch_content_type != SIGIL_SWITCH_CONTENT_UNKNOWN || r.title_version != 0) {
        fprintf(stderr, "FAIL %s: content type %d, version %u: a ticket gives neither\n",
                label, r.switch_content_type, (unsigned)r.title_version);
        failures++;
    }
}

int main(void) {
    memset(junk_nca, 0x5A, sizeof junk_nca);
    memcpy(plain_nca + 0x200, "NCA3", 4);
    static const uint8_t program_id[8] = { 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01 };
    memcpy(plain_nca + 0x210, program_id, 8); /* 0100000000010000, little-endian */

    uint8_t key[32];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
    sigil_support sup;
    memset(&sup, 0, sizeof sup);
    sup.struct_version = SIGIL_SUPPORT_V1;
    sup.switch_header_key = key;

    sigil_options no_key;
    memset(&no_key, 0, sizeof no_key);
    no_key.struct_version = SIGIL_OPTIONS_V1;
    sigil_options with_key = no_key;
    with_key.support = &sup;

    const char *game = "0100AC10085CE000";
    ticket t_game = rsa2048("0100ac10085ce0000000000000000003");

    /* One game ticket, as 77 of the maintainer's NSPs carry. */
    {
        member m[] = { nca("3673a2dd4613c48577a9c22c00874f1d.nca"),
                       nca("f9764404d64f8dd077725630f56dca68.cnmt.nca"),
                       tik("0100ac10085ce0000000000000000003.tik", &t_game) };
        build_nsp(m, 3);
        expect("nsp, no keys", "game.nsp", &no_key, SIGIL_OK, game, game);
        expect("nsp, default options", "game.nsp", NULL, SIGIL_OK, game, game);
        /* Keys that don't open the content still say so. */
        expect("nsp, keys that don't fit", "game.nsp", &with_key, SIGIL_ERR_KEYS_INCOMPATIBLE, NULL, NULL);
    }

    /* With keys, the keys decide: the NCA's program id, not the ticket's. */
    {
        member m[] = { { "game.nca", plain_nca, sizeof plain_nca, 0 },
                       tik("0100ac10085ce0000000000000000003.tik", &t_game) };
        build_nsp(m, 2);
        expect("keys decide", "game.nsp", &with_key, SIGIL_OK, "0100000000010000", "0100000000010000");
        expect("no keys, plaintext NCA unread", "game.nsp", &no_key, SIGIL_OK, game, game);
    }

    /* The id comes from the ticket's body, not its file name. */
    {
        member m[] = { nca("a.nca"), tik("01001520000220000000000000000000.tik", &t_game) };
        build_nsp(m, 2);
        expect("name ignored", "game.nsp", &no_key, SIGIL_OK, game, game);
    }

    /* What isn't a ticket Eden loads gives no id, in a package or on a card. */
    {
        member m[] = { nca("a.nca"), nca("b.cnmt.nca") };
        refused("no ticket", &no_key, m, 2);
        build_nsp(m, 2);
        memcpy(g_img, "XXXX", 4);
        expect("neither PFS0 nor XCI", "game.nsp", &no_key, SIGIL_ERR_NEEDS_KEY, NULL, NULL);
    }
    {
        member m[] = { nca("a.nca"), tik("0100ac10085ce0000000000000000003.TIK", &t_game) };
        refused("extension not exactly tik", &no_key, m, 2);
    }
    {
        ticket t = make_ticket(0x10006, 0x14 + 0x28, "0100ac10085ce0000000000000000003");
        member m[] = { nca("a.nca"), tik("0100ac10085ce0000000000000000003.tik", &t) };
        refused("signature type Eden doesn't read", &no_key, m, 2);
    }
    {
        ticket t = t_game;
        memset(t.bytes + 4 + t.sig, 0, 0x40);
        member m[] = { nca("a.nca"), tik("0100ac10085ce0000000000000000003.tik", &t) };
        refused("all-zero issuer", &no_key, m, 2);
    }
    {
        ticket t = rsa2048("00000000000000000000000000000000");
        member m[] = { nca("a.nca"), tik("00000000000000000000000000000000.tik", &t) };
        refused("all-zero rights id", &no_key, m, 2);
    }
    {
        ticket t = rsa2048("0400e88025b1a0000000000000000014");
        member m[] = { nca("a.nca"), tik("0400e88025b1a0000000000000000014.tik", &t) };
        refused("rights id not 01", &no_key, m, 2);
    }
    {
        /* The file after it fills in the rights id's last byte. */
        ticket t = t_game;
        t.len = 4 + t.sig + 0x16F;
        member m[] = { tik("0100ac10085ce0000000000000000003.tik", &t), nca("a.nca") };
        refused("ticket ends before its rights id", &no_key, m, 2);
    }

    /* Each signature size puts the ticket data elsewhere. */
    {
        ticket t4096 = make_ticket(0x10003, SIG_RSA4096, "0100ac10085ce0000000000000000003");
        member m[] = { nca("a.nca"), tik("0100ac10085ce0000000000000000003.tik", &t4096) };
        build_nsp(m, 2);
        expect("RSA-4096 ticket", "game.nsp", &no_key, SIGIL_OK, game, game);
    }
    {
        ticket tec = make_ticket(0x10005, SIG_ECDSA, "0100ac10085ce0000000000000000003");
        member m[] = { nca("a.nca"), tik("0100ac10085ce0000000000000000003.tik", &tec) };
        build_nsp(m, 2);
        expect("ECDSA ticket", "game.nsp", &no_key, SIGIL_OK, game, game);
    }

    /* More than one ticket: every one must name the same game. */
    ticket psy = rsa2048("0100ec100a7900000000000000000004");
    ticket psy_dlc = rsa2048("0100ec100a7910010000000000000005");
    ticket psy_update = rsa2048("0100ec100a7908000000000000000005");
    {
        member m[] = { nca("a.nca"), tik("0100ec100a7900000000000000000004.tik", &psy),
                       nca("b.nca"), tik("0100ec100a7910010000000000000005.tik", &psy_dlc) };
        build_nsp(m, 4);
        expect("game and DLC", "game.nsp", &no_key, SIGIL_OK, "0100EC100A790000", "0100EC100A790000");
    }
    {
        member m[] = { nca("a.nca"), tik("0100ec100a7910010000000000000005.tik", &psy_dlc),
                       tik("0100ec100a7900000000000000000004.tik", &psy) };
        build_nsp(m, 3);
        expect("game's ticket after the DLC's", "game.nsp", &no_key, SIGIL_OK,
               "0100EC100A790000", "0100EC100A790000");
    }
    {
        /* An NSP can hold an update alone, so its content is the update. */
        ticket contra = rsa2048("0100dca00da7e8000000000000000008");
        member m[] = { nca("a.nca"), tik("0100dca00da7e8000000000000000008.tik", &contra) };
        build_nsp(m, 2);
        expect("update only", "game.nsp", &no_key, SIGIL_OK, "0100DCA00DA7E000", "0100DCA00DA7E800");
    }
    {
        member m[] = { tik("0100ec100a7908000000000000000005.tik", &psy_update),
                       tik("0100ec100a7910010000000000000005.tik", &psy_dlc) };
        build_nsp(m, 2);
        expect("update and DLC, no game ticket", "game.nsp", &no_key, SIGIL_OK,
               "0100EC100A790000", "0100EC100A790800");
    }
    {
        member m[] = { tik("0100ec100a7900000000000000000004.tik", &psy),
                       tik("0100ac10085ce0000000000000000003.tik", &t_game) };
        refused("two games", &no_key, m, 2);
    }
    {
        member m[] = { tik("0100ec100a7908000000000000000005.tik", &psy_update),
                       tik("0100ac10085ce0000000000000000003.tik", &t_game) };
        refused("an update of one game and another game", &no_key, m, 2);
    }

    /* NSZ keeps its tickets beside the compressed NCAs. */
    {
        member m[] = { nca("3673a2dd4613c48577a9c22c00874f1d.ncz"),
                       nca("f9764404d64f8dd077725630f56dca68.cnmt.nca"),
                       tik("0100ac10085ce0000000000000000003.tik", &t_game) };
        build_nsp(m, 3);
        expect("nsz", "game.nsz", &no_key, SIGIL_OK, game, game);
    }

    /* A card holds its game, and a ticket only for an update bundled with
     * it: the content is the game, whichever ticket names it. */
    {
        member m[] = { nca("a.nca"), nca("b.cnmt.nca"), nca("c.nca"),
                       tik("0100ec100a7908000000000000000005.tik", &psy_update),
                       { "0100ec100a7908000000000000000005.cert", junk_nca, 0x700, 0 } };
        build_xci(m, 5);
        expect("xci with an update's ticket", "game.xci", &no_key, SIGIL_OK,
               "0100EC100A790000", "0100EC100A790000");
        expect("xci with an update's ticket, default options", "game.xci", NULL, SIGIL_OK,
               "0100EC100A790000", "0100EC100A790000");
        expect("xci, keys that don't fit", "game.xci", &with_key, SIGIL_ERR_KEYS_INCOMPATIBLE,
               NULL, NULL);
    }
    {
        member m[] = { nca("a.nca"), nca("b.cnmt.nca"),
                       tik("0100ac10085ce0000000000000000003.tik", &t_game) };
        build_xci(m, 3);
        expect("xci with the game's ticket", "game.xci", &no_key, SIGIL_OK, game, game);
    }
    {
        member m[] = { nca("a.nca"), tik("0100ec100a7910010000000000000005.tik", &psy_dlc),
                       tik("0100ec100a7908000000000000000005.tik", &psy_update) };
        build_xci(m, 3);
        expect("xci with a DLC's and an update's ticket", "game.xci", &no_key, SIGIL_OK,
               "0100EC100A790000", "0100EC100A790000");
    }
    {
        member m[] = { nca("a.ncz"), nca("b.cnmt.nca"),
                       tik("0100ec100a7908000000000000000005.tik", &psy_update) };
        build_xci(m, 3);
        expect("xcz with an update's ticket", "game.xcz", &no_key, SIGIL_OK,
               "0100EC100A790000", "0100EC100A790000");
    }

    if (failures) return 1;
    printf("unit_switch_ticket: all passed\n");
    return 0;
}
