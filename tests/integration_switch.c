// SPDX-License-Identifier: MPL-2.0
#include "integration_helpers.h"

/* NULL when no prod.keys is at hand: only the checks without keys run. */
static const char *g_prod_keys = NULL;
static int g_keys_too_old = 0;
static int g_by_ticket = 0;
static int g_needs_key = 0;
static int g_unsound = 0;

static bool is_switch_title_id(const char *id) {
    /* Switch canonical: 16 hex chars starting with "01". */
    return strlen(id) == 16 && id[0] == '0' && id[1] == '1';
}

/* Without keys a package gives the game its tickets name, read from file
 * content and with no content type, or SIGIL_ERR_NEEDS_KEY when it carries
 * none. Returns 0 when the read is one of those; *read is true for the first. */
static int check_without_keys(const char *path, const char *name, sigil_result *r, bool *read) {
    sigil_options opts = { .struct_version = SIGIL_OPTIONS_V1 };
    int rc = sigil_extract_from_path(path, SIGIL_PLATFORM_SWITCH, &opts, r);
    *read = rc == SIGIL_OK;
    if (rc == SIGIL_ERR_NEEDS_KEY) {
        g_needs_key++;
        return 0;
    }
    if (rc != SIGIL_OK) {
        fprintf(stderr, "  FAIL %s without keys: %s\n", name, sigil_strerror(rc));
        g_unsound++;
        return -1;
    }
    if (!is_switch_title_id(r->title_id) || !is_switch_title_id(r->raw_serial)
        || r->source != SIGIL_SOURCE_BINARY
        || r->switch_content_type != SIGIL_SWITCH_CONTENT_UNKNOWN) {
        fprintf(stderr, "  FAIL %s without keys: title_id=%s raw_serial=%s source=%d content type=%d\n",
                name, r->title_id, r->raw_serial, (int)r->source, r->switch_content_type);
        g_unsound++;
        return -1;
    }
    g_by_ticket++;
    return 0;
}

static int check(const char *path, const char *name) {
    sigil_result ticket;
    bool by_ticket;
    if (check_without_keys(path, name, &ticket, &by_ticket) != 0) return -1;
    if (!g_prod_keys) {
        fprintf(stdout, "  ok  %s -> %s\n", name, by_ticket ? ticket.raw_serial : "needs keys");
        return 0;
    }

    sigil_support sup = {
        .struct_version = SIGIL_SUPPORT_V1,
        .switch_prod_keys_path = g_prod_keys,
    };
    sigil_options opts = {
        .struct_version = SIGIL_OPTIONS_V1,
        .support = &sup,
        .flags = SIGIL_FLAG_FILENAME_FALLBACK,
    };
    sigil_result r;
    int rc = sigil_extract_from_path(path, SIGIL_PLATFORM_SWITCH, &opts, &r);
    if (rc == SIGIL_ERR_KEYS_INCOMPATIBLE) {
        /* The key file predates the dump's key generation: the environment,
         * not sigil. Counted apart and left out of the pass rate. */
        fprintf(stdout, "  keys %s: %s\n", name, sigil_strerror(rc));
        g_keys_too_old++;
        return 0;
    }
    if (rc != SIGIL_OK) {
        fprintf(stderr, "  FAIL %s: %s\n", name, sigil_strerror(rc));
        return -1;
    }
    if (!is_switch_title_id(r.title_id)) {
        fprintf(stderr, "  FAIL %s: title_id=%s\n", name, r.title_id);
        return -1;
    }
    if (r.source != SIGIL_SOURCE_BINARY) {
        fprintf(stderr, "  FAIL %s: source=filename (want binary — wrong tweak math?)\n", name);
        return -1;
    }
    /* The tickets name the game and the content the CNMTs do. */
    if (by_ticket && (strcmp(ticket.title_id, r.title_id) != 0
                      || strcmp(ticket.raw_serial, r.raw_serial) != 0)) {
        fprintf(stderr, "  FAIL %s: keys give %s (%s), the tickets %s (%s)\n",
                name, r.title_id, r.raw_serial, ticket.title_id, ticket.raw_serial);
        g_unsound++;
        return -1;
    }
    fprintf(stdout, "  ok  %s -> %s%s\n", name, r.title_id, by_ticket ? ", tickets agree" : "");
    return 0;
}

int main(void) {
    const char *rom_dir = get_rom_dir();
    if (!rom_dir) { fprintf(stderr, "SIGIL_ROM_DIR not set; skipping\n"); return TEST_SKIP; }

    /* The CNMT checks need prod.keys. Default to the bundled path under
     * /tmp/roms/bios/switch/prod.keys; allow override. Without it only the
     * checks without keys run. */
    g_prod_keys = getenv("SIGIL_PROD_KEYS");
    if (!g_prod_keys) g_prod_keys = "/tmp/roms/bios/switch/prod.keys";
    FILE *fp = fopen(g_prod_keys, "rb");
    if (fp) {
        fclose(fp);
    } else {
        fprintf(stdout, "prod.keys missing at %s; checking without keys only\n", g_prod_keys);
        g_prod_keys = NULL;
    }

    char path[512];
    if (build_subdir(rom_dir, "switch", path) != 0) return 1;
    const char *exts[] = { "xci", "nsp", "xcz", "nsz", NULL };
    walk_stats st = walk_dir(path, check, exts);
    int read = st.passed - g_keys_too_old;
    fprintf(stdout, "Switch: processed=%d passed=%d failed=%d keys too old=%d; "
            "without keys: %d by ticket, %d need keys\n",
            st.processed, read, st.failed, g_keys_too_old, g_by_ticket, g_needs_key);
    if (st.processed == 0) { fprintf(stderr, "no Switch samples\n"); return TEST_SKIP; }
    if (g_unsound) return 1;
    if (!g_prod_keys) return 0;
    int threshold = ((st.processed - g_keys_too_old) * 4) / 5;
    return read >= threshold ? 0 : 1;
}
