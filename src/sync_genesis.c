// SPDX-License-Identifier: MPL-2.0
/* Collect and restore for Mega Drive, 32X, Master System and Game Gear carts.
 * Genesis Plus GX and PicoDrive keep a cart's save RAM at the same offsets,
 * the byte at each address the game wrote it to, and differ in what they
 * fill the rest with and how much of it they save:
 *
 * - Genesis Plus GX keeps the cart's 64 KiB save window (core/cart_hw/sram.c,
 *   eeprom_i2c.c, sms_cart.c), 0xFF where the game never wrote, and saves up
 *   to the last byte that isn't 0xFF (libretro.c retro_get_memory_size).
 * - PicoDrive keeps the window from the save's start address at the size the
 *   ROM header gives, 8 KiB for an EEPROM and 32 KiB for every Master System
 *   and Game Gear cart (pico/cart.c PicoCartDetect, PicoCartDetectMS), 0x00
 *   where the game never wrote, and saves all of it.
 *
 * A frontend reads a shorter file into the start of the core's RAM and leaves
 * the rest as the core filled it. The unit is the RAM as the emulator's file
 * holds it. Its identity leaves out trailing 0xFF, so a save has one identity
 * in either core. Restore writes Genesis Plus GX's file without the trailing
 * 0xFF, and PicoDrive's padded with 0xFF to PicoDrive's size, so the game
 * reads in PicoDrive the bytes it read in Genesis Plus GX: 32 KiB on Master
 * System and Game Gear; on Mega Drive and 32X the size of the file already
 * there when it holds the whole save, else the unit's own length. Neither
 * file runs past the RAM its core keeps. Restore also takes a zip of the one
 * save, under RomM's neutral name save.sram or an emulator's. */
#include "sync_internal.h"

/* More than either core keeps for any cart: Genesis Plus GX caps the window
 * at 64 KiB, and PicoDrive's widest range for a real cart (carthw.cfg) is
 * 1 MiB. */
#define SEGA_MAX_RAM   (1024u * 1024u)
#define SEGA_GPGX_RAM  0x10000u  /* Genesis Plus GX's save window */
#define SEGA_8BIT_RAM  0x8000u   /* PicoDrive's Master System and Game Gear size */
#define SEGA_SRAM_NAME "save.sram"

typedef enum { FORM_AS_IS, FORM_GPGX, FORM_PICODRIVE } sega_form;

/* The form a core keeps the save in, by layout id (Genesis Plus GX Wide saves
 * as Genesis Plus GX does); a core sigil hasn't read keeps the bytes as they
 * came. */
static sega_form form_of(const char *layout) {
    if (!layout) return FORM_AS_IS;
    if (strcmp(layout, "genesis_plus_gx") == 0 || strcmp(layout, "genesis_plus_gx_wide") == 0) return FORM_GPGX;
    if (strcmp(layout, "picodrive") == 0) return FORM_PICODRIVE;
    return FORM_AS_IS;
}

/* The length up to the last byte that isn't 0xFF, as Genesis Plus GX saves. */
static size_t trimmed_len(const uint8_t *ram, size_t len) {
    while (len && ram[len - 1] == 0xFF) len--;
    return len;
}

static void identity_of(const uint8_t *ram, size_t len, char out[33]) {
    sigil_md5_of(ram, trimmed_len(ram, len), out);
}

/* PicoDrive's size for the save `ram`, 0 when sigil can't tell it. On Master
 * System and Game Gear it is 32 KiB for every cart (pico/cart.c
 * PicoCartDetectMS). On Mega Drive and 32X it comes from the ROM header and
 * carthw.cfg (PicoCartDetect), which sigil doesn't read, so it is the length
 * of the file already there, and only when that file holds the whole save:
 * the file is PicoDrive's size only if PicoDrive wrote it, and in a saves
 * folder both cores share Genesis Plus GX may have written it, trimmed. */
static size_t picodrive_size(const sigil_sync_request *req, const uint8_t *ram, size_t len, size_t local_len) {
    const char *slug = sigil_layout_platform(req->save.platform);
    if (strcmp(slug, "sms") == 0 || strcmp(slug, "gamegear") == 0) return SEGA_8BIT_RAM;
    return local_len >= trimmed_len(ram, len) ? local_len : 0;
}

/* The bytes restore writes: the RAM in the form of the request's core, never
 * longer than the RAM the core keeps, since a byte past it is one the game
 * can't read there. A RAM of 0xFF alone keeps its length for Genesis Plus GX,
 * which reads the same either way, so the file isn't empty. */
static int file_bytes(const sigil_sync_request *req, const uint8_t *ram, size_t len, size_t local_len, uint8_t **out,
                      size_t *out_len) {
    switch (form_of(req->save.layout)) {
    case FORM_GPGX: {
        size_t kept = len < SEGA_GPGX_RAM ? len : SEGA_GPGX_RAM;
        size_t used = trimmed_len(ram, kept);
        *out_len = used ? used : kept;
        break;
    }
    case FORM_PICODRIVE: {
        size_t size = picodrive_size(req, ram, len, local_len);
        *out_len = size ? size : len;
        break;
    }
    default:
        *out_len = len;
        break;
    }
    *out = (uint8_t *)malloc(*out_len ? *out_len : 1);
    if (!*out) return SIGIL_ERR_OOM;
    memset(*out, 0xFF, *out_len);
    memcpy(*out, ram, len < *out_len ? len : *out_len);
    return SIGIL_OK;
}

/* The RAM a unit carries: the raw file, or a zip of the one save under RomM's
 * neutral name `save.sram` or as an emulator's `.srm` or `.sav`. `*ram` is
 * malloc'd. */
static int read_unit(const uint8_t *unit, size_t len, size_t received_len, uint8_t **ram, size_t *ram_len,
                     sigil_sync_result *r) {
    *ram = NULL;
    *ram_len = 0;
    if (len < 4 || sigil_read_le32(unit) != 0x04034b50u) {
        if (!len) return SIGIL_ERR_NOT_FOUND;
        if (len > SEGA_MAX_RAM) return SIGIL_ERR_UNSUPPORTED_FORMAT;
        sigil_md5_of(unit, received_len, r->content_hash);
        r->shape = SIGIL_SAVE_SHAPE_SINGLE;
        *ram = (uint8_t *)malloc(len);
        if (!*ram) return SIGIL_ERR_OOM;
        memcpy(*ram, unit, len);
        *ram_len = len;
        return SIGIL_OK;
    }
    sigil_zip_member *members = NULL;
    size_t count = 0;
    int rc = sigil_zip_read_mem(unit, len, SEGA_MAX_RAM, &members, &count);
    if (rc == SIGIL_OK && count != 1) rc = count ? SIGIL_ERR_UNSUPPORTED_FORMAT : SIGIL_ERR_NOT_FOUND;
    if (rc == SIGIL_OK) {
        const sigil_zip_member *m = &members[0];
        if (strcmp(m->name, SEGA_SRAM_NAME) != 0 && !sigil_ends_with(m->name, ".srm") &&
            !sigil_ends_with(m->name, ".sav")) {
            snprintf(r->problem, sizeof(r->problem), "%s", m->name);
            rc = SIGIL_ERR_UNSUPPORTED_FORMAT;
        } else if (!m->len) {
            rc = SIGIL_ERR_NOT_FOUND;
        } else {
            sigil_named_md5 part;
            snprintf(part.name, sizeof(part.name), "%s", m->name);
            sigil_md5_of(m->data, m->len, part.md5);
            sigil_named_hash(&part, 1, r->content_hash);
            r->shape = SIGIL_SAVE_SHAPE_MULTI;
            *ram = members[0].data;
            *ram_len = m->len;
            members[0].data = NULL;
        }
    }
    if (members) sigil_zip_members_free(members, count);
    return rc;
}

int sigil_sync_collect_genesis(sigil_sync_ctx *x, sigil_sync_result *r) {
    sigil_sync_cart_file f;
    uint8_t *ram = NULL;
    size_t len = 0;
    int rc = sigil_sync_cart_file_of(x->req, &f);
    if (rc == SIGIL_OK) rc = sigil_sync_cart_read(x->req, &f, SEGA_MAX_RAM, &ram, &len);
    if (rc == SIGIL_OK && ram) {
        identity_of(ram, len, r->identity_hash);
        sigil_md5_of(ram, len, r->content_hash);
        r->data = ram;
        r->len = len;
        r->shape = SIGIL_SAVE_SHAPE_SINGLE;
        sigil_sync_cart_artifact(x->req, ".srm", r);
        ram = NULL;
    }
    free(ram);
    return rc;
}

int sigil_sync_restore_genesis(sigil_sync_ctx *x, const uint8_t *unit, size_t len, size_t received_len,
                               sigil_sync_result *r, char local_identity[33]) {
    uint8_t *ram = NULL;
    size_t ram_len = 0;
    int rc = read_unit(unit, len, received_len, &ram, &ram_len, r);
    if (rc != SIGIL_OK) return rc;
    identity_of(ram, ram_len, r->identity_hash);
    sigil_sync_cart_artifact(x->req, ".srm", r);

    sigil_sync_cart_file f;
    uint8_t *local = NULL;
    size_t local_len = 0;
    rc = sigil_sync_cart_file_of(x->req, &f);
    if (rc == SIGIL_OK) rc = sigil_sync_cart_read(x->req, &f, SEGA_MAX_RAM, &local, &local_len);
    if (rc == SIGIL_OK && local) identity_of(local, local_len, local_identity);
    free(local);
    bool already = false;
    if (rc == SIGIL_OK && sigil_sync_blocks_restore(x, local_identity, r->identity_hash,
                                                    sigil_sync_state_get(&x->state, "synced", x->game), &already)) {
        r->conflict = 1;
        rc = SIGIL_ERR_CONFLICT;
    }
    if (rc == SIGIL_OK && !f.path[0]) {
        snprintf(r->problem, sizeof(r->problem), "%s", r->artifact);
        rc = SIGIL_ERR_NO_TARGET;
    }
    /* The same save in the other core's form is written again, so the file
     * is the one the request's core reads. */
    uint8_t *file = NULL;
    size_t file_len = 0;
    if (rc == SIGIL_OK) rc = file_bytes(x->req, ram, ram_len, local_len, &file, &file_len);
    if (rc == SIGIL_OK && !sigil_sync_file_holds(x->req, f.path, file, file_len)) {
        rc = sigil_sync_put(x->req, f.path, file, file_len);
    }
    free(file);
    free(ram);
    return rc;
}
