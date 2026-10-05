// SPDX-License-Identifier: MPL-2.0
/* Game Boy and Game Boy Color sync against saves from real carts: the RAM
 * travels as it is, a clock cart's clock in the neutral layout, and restore
 * writes each core's own clock file. */
#include "save_corpus.h"
#include "legacy_units.h"
#include "clock_gb.h"

#define TEST_SKIP 77
#define CRYSTAL "Pokemon - Crystal Version (USA, Europe) (Rev 1)"

static int g_fails = 0;

static void fail(const char *where, const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", where, what);
    g_fails++;
}

static corpus_table g_gb, g_gbc;

typedef struct {
    sigil_sync_request req;
    sigil_result       result;
    const char        *ids[1];
} game;

static void make_game(game *g, mem_root *root, const char *layout, const char *platform, const char *content,
                      bool clock) {
    memset(g, 0, sizeof(*g));
    g->result.struct_version = SIGIL_RESULT_V4;
    g->result.platform = strcmp(platform, "gbc") == 0 ? SIGIL_PLATFORM_GBC : SIGIL_PLATFORM_GB;
    g->result.features = clock ? SIGIL_FEATURE_RTC : 0;
    g->ids[0] = content;
    g->req.struct_version = SIGIL_SYNC_REQUEST_V1;
    g->req.save.struct_version = SIGIL_SAVE_REQUEST_V1;
    g->req.save.layout = layout;
    g->req.save.platform = platform;
    g->req.save.content_path = content;
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

static sigil_sync_result *collect_crystal(mem_root *root, const char *layout) {
    game g;
    make_game(&g, root, layout, "gbc", CRYSTAL ".gbc", true);
    sigil_sync_result *r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK) {
        sigil_sync_result_free(r);
        return NULL;
    }
    return r;
}

static sigil_sync_result *restore_crystal(mem_root *root, const char *layout, const uint8_t *unit, size_t len, int *rc) {
    game g;
    make_game(&g, root, layout, "gbc", CRYSTAL ".gbc", true);
    g.req.overwrite_local = 1;
    sigil_sync_result *w = NULL;
    *rc = sigil_restore(&g.req, unit, len, &w);
    return w;
}

static uint8_t *member_of(const sigil_sync_result *r, const char *name, size_t *len) {
    sigil_zip_member *m = NULL;
    size_t n = 0;
    uint8_t *out = NULL;
    if (!r || !r->data || sigil_zip_read_mem(r->data, r->len, 1u << 20, &m, &n) != SIGIL_OK) return NULL;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(m[i].name, name) != 0) continue;
        out = (uint8_t *)malloc(m[i].len ? m[i].len : 1);
        memcpy(out, m[i].data, m[i].len);
        *len = m[i].len;
    }
    sigil_zip_members_free(m, n);
    return out;
}

/* The instant a clock's counter was zero: what any two forms of one clock agree on. */
static int64_t clock_base(const sigil_gb_clock *c) {
    return c->stamp - ((int64_t)c->days * 86400 + c->hours * 3600 + c->minutes * 60 + c->seconds);
}

static bool unit_clock(const sigil_sync_result *r, sigil_gb_clock *c) {
    size_t len = 0;
    bool lossy = false;
    uint8_t *rtc = member_of(r, "clock.rtc", &len);
    bool ok = rtc && sigil_gb_clock_read(SIGIL_GB_CLOCK_VBA, rtc, len, 0, c, &lossy) == SIGIL_OK;
    free(rtc);
    return ok;
}

/* A cart with no clock: the unit is its RAM, as it is. */
static void check_plain_cart(void) {
    size_t len = 0;
    uint8_t *ram = corpus_sample(&g_gb, "gb", "pokemon-blue-gambatte", NULL, &len);
    if (!ram) return;
    mem_root root = {0};
    root_put(&root, "Pokemon Blue.srm", ram, len);
    game g;
    make_game(&g, &root, "gambatte", "gb", "Pokemon Blue.gb", false);
    sigil_sync_result *r = NULL;
    if (sigil_collect(&g.req, &r) != SIGIL_OK || !r->data || r->len != len || memcmp(r->data, ram, len) != 0 ||
        r->shape != SIGIL_SAVE_SHAPE_SINGLE || strcmp(r->artifact, "Pokemon Blue.srm") != 0) {
        fail("plain cart", "the unit is the RAM as it is, under the content's name");
    } else {
        mem_root fresh = {0};
        game t;
        make_game(&t, &fresh, "mgba", "gb", "Pokemon Blue.gb", false);
        sigil_sync_result *w = NULL;
        mem_file *f = NULL;
        if (sigil_restore(&t.req, r->data, r->len, &w) != SIGIL_OK || !(f = root_find(&fresh, "Pokemon Blue.srm")) ||
            f->len != len || memcmp(f->data, ram, len) != 0 || fresh.count != 1) {
            fail("plain cart", "restore into another core writes the .srm alone");
        }
        sigil_sync_result_free(w);
        root_free(&fresh);
    }
    sigil_sync_result_free(r);
    root_free(&root);
    free(ram);
}

/* A clock cart: save.sram and clock.rtc, the same clock from the mGBA pair and from a standalone .sav. */
static void check_clock_cart(void) {
    size_t ram_len = 0, rtc_len = 0, sav_len = 0;
    uint8_t *ram = corpus_sample(&g_gbc, "gbc", "crystal-mgba", CRYSTAL ".srm", &ram_len);
    uint8_t *rtc = corpus_sample(&g_gbc, "gbc", "crystal-mgba", CRYSTAL ".rtc", &rtc_len);
    uint8_t *sav = corpus_sample(&g_gbc, "gbc", "crystal-sav-footer-constructed", NULL, &sav_len);
    if (!ram || !rtc || !sav) {
        free(ram);
        free(rtc);
        free(sav);
        return;
    }
    mem_root root = {0};
    root_put(&root, CRYSTAL ".srm", ram, ram_len);
    root_put(&root, CRYSTAL ".rtc", rtc, rtc_len);
    sigil_sync_result *unit = collect_crystal(&root, "mgba");
    size_t got_len = 0;
    uint8_t *got = member_of(unit, "save.sram", &got_len);
    sigil_gb_clock want, have;
    bool lossy = false;
    sigil_gb_clock_read(SIGIL_GB_CLOCK_MGBA, rtc, rtc_len, 0, &want, &lossy);
    if (!unit || unit->shape != SIGIL_SAVE_SHAPE_MULTI || !got || got_len != ram_len || memcmp(got, ram, ram_len) != 0 ||
        !unit_clock(unit, &have) || have.stamp != want.stamp || have.seconds != want.seconds || have.hours != want.hours) {
        fail("clock cart", "save.sram is the RAM, clock.rtc the mGBA clock");
    }
    free(got);
    sigil_named_md5 part = { "save.sram", "" };
    char identity[33];
    sigil_md5_of(ram, ram_len, part.md5);
    sigil_named_hash(&part, 1, identity);
    if (unit && strcmp(unit->identity_hash, identity) != 0) fail("clock cart", "identity isn't RomM's zip hash over save.sram");

    /* mGBA pairs the time with its latched registers: those are the clock. */
    uint8_t latched_rtc[48];
    memcpy(latched_rtc, rtc, sizeof(latched_rtc));
    sigil_write_le32(latched_rtc + 20, 7);   /* latched seconds differ from current */
    mem_root mgba = {0};
    root_put(&mgba, CRYSTAL ".srm", ram, ram_len);
    root_put(&mgba, CRYSTAL ".rtc", latched_rtc, sizeof(latched_rtc));
    sigil_sync_result *paired = collect_crystal(&mgba, "mgba");
    if (!unit_clock(paired, &have) || have.seconds != 7) fail("mgba clock", "the latched registers aren't read as the clock");
    sigil_sync_result_free(paired);
    root_free(&mgba);

    /* The single .sav a standalone emulator writes restores as the unit does, into SameBoy's 32-byte clock. */
    int rc_unit = 0, rc_sav = 0;
    mem_root a = {0}, b = {0};
    sigil_sync_result *wa = unit ? restore_crystal(&a, "sameboy", unit->data, unit->len, &rc_unit) : NULL;
    sigil_sync_result *wb = restore_crystal(&b, "sameboy", sav, sav_len, &rc_sav);
    mem_file *clock = root_find(&a, CRYSTAL ".rtc");
    if (rc_unit != SIGIL_OK || !clock || clock->len != 32) fail("clock cart", "SameBoy gets its 32-byte clock");
    if (rc_sav != SIGIL_OK || !roots_same(&a, &b)) fail("standalone .sav", "restores other files than the unit");
    sigil_sync_result_free(wa);
    sigil_sync_result_free(wb);

    /* Into gambatte and back: the same RAM, the clock at the same base time. */
    mem_root gam = {0};
    int rc = 0;
    sigil_sync_result *w = unit ? restore_crystal(&gam, "gambatte", unit->data, unit->len, &rc) : NULL;
    clock = root_find(&gam, CRYSTAL ".rtc");
    sigil_sync_result *back = collect_crystal(&gam, "gambatte");
    if (rc != SIGIL_OK || !clock || clock->len != 8 || !back || !unit ||
        strcmp(back->identity_hash, unit->identity_hash) != 0 || !unit_clock(back, &have) ||
        clock_base(&have) != clock_base(&want)) {
        fail("gambatte", "an 8-byte clock at the same base time, the same RAM collected back");
    }
    sigil_sync_result_free(back);
    sigil_sync_result_free(w);

    /* A core sigil doesn't know: the clock goes in the format of the .rtc there; with none, only the RAM. */
    mem_root unknown = {0}, mesen = {0};
    w = unit ? restore_crystal(&unknown, "bsnes", unit->data, unit->len, &rc) : NULL;
    if (rc != SIGIL_OK || unknown.count != 1 || !root_find(&unknown, CRYSTAL ".srm")) {
        fail("unknown core", "only the .srm, no clock file in a format sigil can't know");
    }
    sigil_sync_result_free(w);
    uint8_t thirteen[13] = { 0 };
    root_put(&mesen, CRYSTAL ".rtc", thirteen, sizeof(thirteen));
    w = unit ? restore_crystal(&mesen, "bsnes", unit->data, unit->len, &rc) : NULL;
    clock = root_find(&mesen, CRYSTAL ".rtc");
    if (rc != SIGIL_OK || !clock || clock->len != 13 || clock->data[1] != want.minutes) {
        fail("unknown core", "the 13-byte clock there gets the unit's clock");
    }
    sigil_sync_result_free(w);

    /* Argosy's zip of the mGBA .srm and .rtc restores as the unit does. */
    const char *names[] = { CRYSTAL ".srm", CRYSTAL ".rtc" };
    uint8_t *data[] = { ram, rtc };
    size_t lens[] = { ram_len, rtc_len }, zip_len = 0;
    uint8_t *zip = legacy_zip(names, data, lens, 2, &zip_len);
    mem_root from_zip = {0}, from_unit = {0};
    int rc_zip = 0;
    sigil_sync_result *wz = zip ? restore_crystal(&from_zip, "mgba", zip, zip_len, &rc_zip) : NULL;
    sigil_sync_result *wu = unit ? restore_crystal(&from_unit, "mgba", unit->data, unit->len, &rc) : NULL;
    if (rc_zip != SIGIL_OK || !roots_same(&from_zip, &from_unit)) fail("argosy zip", "restores other files than the unit");
    sigil_sync_result_free(wz);
    sigil_sync_result_free(wu);
    free(zip);

    root_free(&from_unit);
    root_free(&from_zip);
    root_free(&mesen);
    root_free(&unknown);
    root_free(&gam);
    root_free(&b);
    root_free(&a);
    sigil_sync_result_free(unit);
    root_free(&root);
    free(ram);
    free(rtc);
    free(sav);
}

/* A clock cart whose emulator kept no clock: the unit holds save.sram alone,
 * with the same identity as one that carries a clock. */
static void check_clock_cart_without_clock(void) {
    size_t ram_len = 0, rtc_len = 0;
    uint8_t *ram = corpus_sample(&g_gbc, "gbc", "crystal-mgba", CRYSTAL ".srm", &ram_len);
    uint8_t *rtc = corpus_sample(&g_gbc, "gbc", "crystal-mgba", CRYSTAL ".rtc", &rtc_len);
    if (!ram || !rtc) {
        free(ram);
        free(rtc);
        return;
    }
    mem_root bare = {0}, paired = {0};
    root_put(&bare, CRYSTAL ".srm", ram, ram_len);
    root_put(&paired, CRYSTAL ".srm", ram, ram_len);
    root_put(&paired, CRYSTAL ".rtc", rtc, rtc_len);
    sigil_sync_result *a = collect_crystal(&bare, "vba_next");
    sigil_sync_result *b = collect_crystal(&paired, "mgba");
    size_t len = 0;
    uint8_t *clock = member_of(a, "clock.rtc", &len);
    if (!a || !b || a->shape != SIGIL_SAVE_SHAPE_MULTI || clock || strcmp(a->identity_hash, b->identity_hash) != 0) {
        fail("clock cart, no clock", "save.sram alone, the same identity as with a clock");
    }

    /* A unit without a clock writes the RAM and leaves the clock on disk alone. */
    uint8_t *older = (uint8_t *)malloc(ram_len);
    memcpy(older, ram, ram_len);
    older[0x100] ^= 0x5A;
    root_put(&paired, CRYSTAL ".srm", older, ram_len);
    free(older);
    int rc = 0;
    sigil_sync_result *w = a ? restore_crystal(&paired, "mgba", a->data, a->len, &rc) : NULL;
    mem_file *kept = root_find(&paired, CRYSTAL ".rtc"), *written = root_find(&paired, CRYSTAL ".srm");
    if (rc != SIGIL_OK || !written || memcmp(written->data, ram, ram_len) != 0) {
        fail("clock cart, no clock", "restore didn't write the RAM");
    }
    if (!kept || kept->len != rtc_len || memcmp(kept->data, rtc, rtc_len) != 0) {
        fail("clock cart, no clock", "restore changed the clock on disk");
    }
    sigil_sync_result_free(w);
    free(clock);
    sigil_sync_result_free(a);
    sigil_sync_result_free(b);
    root_free(&paired);
    root_free(&bare);
    free(ram);
    free(rtc);
}

/* Standalone mGBA, VBA-M, SameBoy and Gearboy: one .sav, the clock appended
 * on a clock cart in each one's pairing, nothing appended otherwise. */
static void check_standalone(void) {
    static const char *const LAYOUTS[] = { "mgba_standalone", "vbam_standalone", "sameboy_standalone", "gearboy_standalone" };
    size_t ram_len = 0, rtc_len = 0, sav_len = 0;
    uint8_t *ram = corpus_sample(&g_gbc, "gbc", "crystal-mgba", CRYSTAL ".srm", &ram_len);
    uint8_t *rtc = corpus_sample(&g_gbc, "gbc", "crystal-mgba", CRYSTAL ".rtc", &rtc_len);
    uint8_t *sav = corpus_sample(&g_gbc, "gbc", "crystal-sav-footer-constructed", NULL, &sav_len);
    if (!ram || !rtc || !sav) {
        free(ram);
        free(rtc);
        free(sav);
        return;
    }
    mem_root pair = {0};
    root_put(&pair, CRYSTAL ".srm", ram, ram_len);
    root_put(&pair, CRYSTAL ".rtc", rtc, rtc_len);
    sigil_sync_result *unit = collect_crystal(&pair, "mgba");
    sigil_gb_clock want;
    bool lossy = false;
    sigil_gb_clock_read(SIGIL_GB_CLOCK_MGBA, rtc, rtc_len, 0, &want, &lossy);

    mem_root vbam = {0};
    root_put(&vbam, CRYSTAL ".sav", sav, sav_len);
    sigil_sync_result *from_sav = collect_crystal(&vbam, "vbam_standalone");
    sigil_gb_clock have;
    if (!unit || !from_sav || strcmp(from_sav->identity_hash, unit->identity_hash) != 0 || !unit_clock(from_sav, &have) ||
        clock_base(&have) != clock_base(&want)) {
        fail("vba-m .sav", "collect gives the same RAM and clock as the mGBA core's pair");
    }
    sigil_sync_result_free(from_sav);

    for (size_t i = 0; unit && i < sizeof(LAYOUTS) / sizeof(LAYOUTS[0]); i++) {
        mem_root root = {0};
        int rc = 0;
        sigil_sync_result *w = restore_crystal(&root, LAYOUTS[i], unit->data, unit->len, &rc);
        mem_file *f = root_find(&root, CRYSTAL ".sav");
        uint8_t footer[48];
        sigil_gb_clock_write(i == 0 ? SIGIL_GB_CLOCK_MGBA : SIGIL_GB_CLOCK_VBA, &want, footer, &lossy);
        if (rc != SIGIL_OK || !f || f->len != ram_len + 48 || memcmp(f->data, ram, ram_len) != 0 ||
            memcmp(f->data + ram_len, footer, 48) != 0 || root.count != 1) {
            fail(LAYOUTS[i], "the .sav is the RAM and the clock in the emulator's footer, nothing else");
        }
        sigil_sync_result *back = collect_crystal(&root, LAYOUTS[i]);
        if (!back || strcmp(back->identity_hash, unit->identity_hash) != 0 || !unit_clock(back, &have) ||
            clock_base(&have) != clock_base(&want)) {
            fail(LAYOUTS[i], "collects back other saves");
        }
        sigil_sync_result_free(back);
        sigil_sync_result_free(w);
        root_free(&root);
    }

    /* mGBA's footer pairs the time with the latched registers, both ways. */
    uint8_t *latched_sav = (uint8_t *)malloc(sav_len);
    memcpy(latched_sav, sav, sav_len);
    sigil_write_le32(latched_sav + ram_len + 20, 7);
    mem_root mgba = {0};
    root_put(&mgba, CRYSTAL ".sav", latched_sav, sav_len);
    sigil_sync_result *paired = collect_crystal(&mgba, "mgba_standalone");
    if (!unit_clock(paired, &have) || have.seconds != 7) fail("mgba .sav", "the latched registers aren't read as the clock");
    mem_root again = {0};
    int rc_again = 0;
    sigil_sync_result *wa = paired ? restore_crystal(&again, "mgba_standalone", paired->data, paired->len, &rc_again) : NULL;
    mem_file *fa = root_find(&again, CRYSTAL ".sav");
    if (rc_again != SIGIL_OK || !fa || fa->len != sav_len || sigil_read_le32(fa->data + ram_len + 20) != 7) {
        fail("mgba .sav", "the clock isn't written back in mGBA's latched slot");
    }
    sigil_sync_result_free(wa);
    sigil_sync_result_free(paired);
    root_free(&again);
    root_free(&mgba);
    free(latched_sav);

    /* A unit without a clock keeps the footer on disk. */
    mem_root kept = {0};
    uint8_t *older = (uint8_t *)malloc(sav_len);
    memcpy(older, sav, sav_len);
    older[0x100] ^= 0x5A;
    root_put(&kept, CRYSTAL ".sav", older, sav_len);
    free(older);
    sigil_named_md5 part = { "save.sram", "" };
    sigil_md5_of(ram, ram_len, part.md5);
    sigil_zip_member bare = { "save.sram", ram, ram_len };
    uint8_t *zip = NULL;
    size_t zip_len = 0;
    sigil_zip_store(&bare, 1, &zip, &zip_len);
    int rc = 0;
    sigil_sync_result *w = restore_crystal(&kept, "sameboy_standalone", zip, zip_len, &rc);
    mem_file *f = root_find(&kept, CRYSTAL ".sav");
    if (rc != SIGIL_OK || !f || f->len != sav_len || memcmp(f->data, sav, sav_len) != 0) {
        fail("sameboy .sav, no clock in the unit", "the RAM written, the clock on disk kept");
    }
    sigil_sync_result_free(w);
    free(zip);
    root_free(&kept);

    /* A .sav with a clock, restored for a cart the request says has none, loses the footer. */
    mem_root plain = {0};
    game p;
    make_game(&p, &plain, "vbam_standalone", "gbc", CRYSTAL ".gbc", false);
    w = NULL;
    f = NULL;
    if (sigil_restore(&p.req, sav, sav_len, &w) != SIGIL_OK || !(f = root_find(&plain, CRYSTAL ".sav")) ||
        f->len != ram_len) {
        fail("vba-m .sav, cart without a clock", "a clock footer written for a cart without a clock");
    }
    sigil_sync_result_free(w);
    root_free(&plain);
    root_free(&vbam);
    sigil_sync_result_free(unit);
    root_free(&pair);
    free(ram);
    free(rtc);
    free(sav);

    /* A cart without a clock gets no footer; mGBA's empty .sav for a cart without RAM is no save. */
    size_t len = 0;
    uint8_t *blue = corpus_sample(&g_gb, "gb", "pokemon-blue-mgba", NULL, &len);
    if (blue) {
        mem_root root = {0};
        game g;
        make_game(&g, &root, "vbam_standalone", "gb", "Pokemon Blue.gb", false);
        g.req.overwrite_local = 1;
        sigil_sync_result *r = NULL;
        mem_file *b = NULL;
        if (sigil_restore(&g.req, blue, len, &r) != SIGIL_OK || !(b = root_find(&root, "Pokemon Blue.sav")) ||
            b->len != len || memcmp(b->data, blue, len) != 0) {
            fail("vba-m .sav, no clock", "the .sav is the RAM alone");
        }
        sigil_sync_result_free(r);
        root_free(&root);
        free(blue);
    }
    mem_root empty = {0};
    root_put(&empty, "Tetris.sav", (const uint8_t *)"", 0);
    game t;
    make_game(&t, &empty, "mgba_standalone", "gb", "Tetris.gb", false);
    sigil_sync_result *none = NULL;
    if (sigil_collect(&t.req, &none) != SIGIL_OK || none->data) fail("mgba empty .sav", "an empty .sav gave a unit");
    sigil_sync_result_free(none);
    root_free(&empty);
}

/* A save changed since the last sync stops the restore until the user agrees. */
static void check_conflict(void) {
    size_t len = 0;
    uint8_t *ram = corpus_sample(&g_gb, "gb", "links-awakening-builtin", NULL, &len);
    if (!ram) return;
    mem_root root = {0};
    root_put(&root, "Zelda.srm", ram, len);
    game g;
    make_game(&g, &root, "gambatte", "gb", "Zelda.gb", false);
    sigil_sync_result *first = NULL, *w = NULL;
    sigil_collect(&g.req, &first);
    uint8_t *other = (uint8_t *)malloc(len);
    memcpy(other, ram, len);
    other[100] ^= 0x5A;
    ram[200] ^= 0x33;
    root_put(&root, "Zelda.srm", ram, len);   /* played on since the last sync */
    g.req.save.listing = root.listing;
    g.req.save.listing_count = root.count;
    g.req.state = first ? first->state : NULL;
    g.req.state_len = first ? first->state_len : 0;
    int writes = root.writes;
    if (sigil_restore(&g.req, other, len, &w) != SIGIL_ERR_CONFLICT || !w || !w->conflict || root.writes != writes) {
        fail("conflict", "a local change was overwritten without asking");
    }
    sigil_sync_result_free(w);
    free(other);
    sigil_sync_result_free(first);
    root_free(&root);
    free(ram);
}

/* Argosy's hardcore uploads: the trailer stays off the cart RAM. */
static void check_hardcore_upload(void) {
    size_t len = 0, marked_len = 0;
    uint8_t *ram = corpus_sample(&g_gb, "gb", "dkl2-builtin", NULL, &len);
    uint8_t *marked = corpus_sample(&g_gb, "gb", "dkl2-builtin-argosy-trailer", NULL, &marked_len);
    if (ram && marked) {
        mem_root root = {0};
        game g;
        make_game(&g, &root, "gambatte", "gb", "DKL2.gb", false);
        sigil_sync_result *w = NULL;
        mem_file *f = NULL;
        if (sigil_restore(&g.req, marked, marked_len, &w) != SIGIL_OK || !w->hardcore_marker ||
            !(f = root_find(&root, "DKL2.srm")) || f->len != len || memcmp(f->data, ram, len) != 0) {
            fail("hardcore upload", "the RAM without the trailer, reported");
        }
        sigil_sync_result_free(w);
        root_free(&root);
    }
    free(ram);
    free(marked);
}

int main(void) {
    char path[1024];
    if (corpus_platform_path("gb", "manifest.tsv", path, sizeof(path)) != 0 || corpus_load(path, &g_gb) != 0 ||
        corpus_platform_path("gbc", "manifest.tsv", path, sizeof(path)) != 0 || corpus_load(path, &g_gbc) != 0) {
        fprintf(stderr, "SKIP: no gb or gbc manifest\n");
        return TEST_SKIP;
    }
    if (!corpus_present(&g_gbc, "gbc", "crystal-mgba")) {
        fprintf(stderr, "SKIP: gb samples missing\n");
        corpus_free(&g_gb);
        corpus_free(&g_gbc);
        return TEST_SKIP;
    }
    check_plain_cart();
    check_clock_cart();
    check_clock_cart_without_clock();
    check_standalone();
    check_conflict();
    check_hardcore_upload();
    corpus_free(&g_gb);
    corpus_free(&g_gbc);
    printf("gb sync: %d failures\n", g_fails);
    return corpus_exit(g_fails);
}
