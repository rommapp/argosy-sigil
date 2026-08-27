// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"
#include <stdlib.h>

/* XDVDFS is the filesystem on both original Xbox and Xbox 360 discs, so this
 * walker serves default.xbe and default.xex alike. Only the partition base
 * differs between disc generations, and that is probed rather than assumed. */

#define XDVDFS_MAGIC          "MICROSOFT*XBOX*MEDIA"
#define XDVDFS_MAGIC_LEN      20
#define XDVDFS_VD_OFF         0x10000
/* The volume descriptor repeats its magic at the end of the same sector.
 * Requiring both is what stops a stray copy of the string inside game data
 * from being read as a partition header. */
#define XDVDFS_VD_TRAILING    0x7EC
#define XDVDFS_VD_ROOT_SECTOR 0x14
#define XDVDFS_VD_ROOT_SIZE   0x18

#define XDVDFS_DIRENT_HEADER  0x0E
#define XDVDFS_ATTR_DIRECTORY 0x10
/* A subtree offset is stored in 4-byte units, and an absent child is 0 or
 * 0xff rather than 0xffff even though the field is 16 bits wide. */
#define XDVDFS_SUBTREE_UNIT   4
#define XDVDFS_NO_CHILD       0xff
/* Root tables are a sector or two. The cap bounds one allocation and one
 * read for an image whose header claims something absurd. */
#define XDVDFS_MAX_TABLE      (512u * 1024u)
/* A corrupt table can point a node at itself. The tree is balanced on write,
 * so no real lookup comes close to this many hops. */
#define XDVDFS_MAX_DEPTH      64

/* Probed in ascending order rather than the reference implementation's
 * XISO/XGD1/XGD2/XGD3, because an archive-backed IO can only seek forward
 * cheaply and an ascending probe never asks it to rewind. XGD2 and XGD3 are
 * 360 disc formats; XGD1 is original Xbox. */
static const uint64_t XDVDFS_PARTITION_BASES[] = {
    0,          /* trimmed xiso */
    34078720,   /* XGD3 */
    265879552,  /* XGD2 */
    405798912   /* XGD1 */
};
static const size_t XDVDFS_BASE_COUNT =
    sizeof(XDVDFS_PARTITION_BASES) / sizeof(XDVDFS_PARTITION_BASES[0]);

static bool xdvdfs_magic_at(const uint8_t *p) {
    return memcmp(p, XDVDFS_MAGIC, XDVDFS_MAGIC_LEN) == 0;
}

/* Locates the game partition and reads its root directory table location.
 * Trimmed images answer at base 0; a full disc image carries a video partition
 * in front of the game partition, so the descriptor sits one XGD offset in. */
static int xdvdfs_find_partition(const sigil_io *io, uint64_t *out_base,
                                 uint32_t *out_root_sector, uint32_t *out_root_size) {
    uint8_t vd[SIGIL_ISO_SECTOR_SIZE];

    for (size_t i = 0; i < XDVDFS_BASE_COUNT; i++) {
        uint64_t base = XDVDFS_PARTITION_BASES[i];
        if (sigil_io_read_exact(io, base + XDVDFS_VD_OFF, vd, sizeof(vd)) != SIGIL_OK) {
            continue;
        }
        if (!xdvdfs_magic_at(vd) || !xdvdfs_magic_at(vd + XDVDFS_VD_TRAILING)) {
            continue;
        }

        uint32_t root_size = sigil_read_le32(vd + XDVDFS_VD_ROOT_SIZE);
        if (root_size == 0 || root_size > XDVDFS_MAX_TABLE) return SIGIL_ERR_NOT_FOUND;

        *out_base        = base;
        *out_root_sector = sigil_read_le32(vd + XDVDFS_VD_ROOT_SECTOR);
        *out_root_size   = root_size;
        return SIGIL_OK;
    }
    return SIGIL_ERR_UNSUPPORTED_FORMAT;
}

/* Ordering used by the on-disk tree: compare ASCII-uppercased, and a name that
 * is a prefix of the other sorts first. Matches cmp_ignore_case_utf8 in the
 * xdvdfs reference; getting it wrong sends the search down the wrong subtree
 * and reports a present file as missing. */
static int xdvdfs_name_cmp(const char *target, const uint8_t *name, size_t name_len) {
    for (size_t i = 0; i < name_len; i++) {
        char t = target[i];
        if (t == '\0') return -1;
        char a = sigil_to_upper(t);
        char b = sigil_to_upper((char)name[i]);
        if (a != b) return (a < b) ? -1 : 1;
    }
    return target[name_len] == '\0' ? 0 : 1;
}

/* Walks the directory tree held in `table` for `target`, returning its data
 * region. The whole table is resident, so the walk never reads backwards
 * through the underlying IO. */
static int xdvdfs_lookup(const uint8_t *table, uint32_t table_size,
                         const char *target,
                         uint32_t *out_sector, uint32_t *out_size) {
    uint32_t off = 0;

    for (int depth = 0; depth < XDVDFS_MAX_DEPTH; depth++) {
        if ((uint64_t)off + XDVDFS_DIRENT_HEADER > table_size) return SIGIL_ERR_NOT_FOUND;

        const uint8_t *node = table + off;

        bool all_ff = true, all_00 = true;
        for (size_t i = 0; i < XDVDFS_DIRENT_HEADER; i++) {
            if (node[i] != 0xFF) all_ff = false;
            if (node[i] != 0x00) all_00 = false;
        }
        if (all_ff || all_00) return SIGIL_ERR_NOT_FOUND;

        uint8_t name_len = node[13];
        if ((uint64_t)off + XDVDFS_DIRENT_HEADER + name_len > table_size) {
            return SIGIL_ERR_NOT_FOUND;
        }

        int cmp = xdvdfs_name_cmp(target, node + XDVDFS_DIRENT_HEADER, name_len);
        if (cmp == 0) {
            if (node[12] & XDVDFS_ATTR_DIRECTORY) return SIGIL_ERR_NOT_FOUND;
            *out_sector = sigil_read_le32(node + 4);
            *out_size   = sigil_read_le32(node + 8);
            return SIGIL_OK;
        }

        uint16_t next = (cmp < 0) ? (uint16_t)(node[0] | (node[1] << 8))
                                  : (uint16_t)(node[2] | (node[3] << 8));
        if (next == 0 || next == XDVDFS_NO_CHILD) return SIGIL_ERR_NOT_FOUND;

        off = (uint32_t)next * XDVDFS_SUBTREE_UNIT;
    }
    return SIGIL_ERR_NOT_FOUND;
}

int sigil_xdvdfs_find_root_file(const sigil_io *io, const char *name,
                                uint64_t *out_off, uint32_t *out_size) {
    if (!io || !io->read || !name || !out_off || !out_size) return SIGIL_ERR_INVALID_ARG;

    uint64_t base;
    uint32_t root_sector, root_size;

    int rc = xdvdfs_find_partition(io, &base, &root_sector, &root_size);
    if (rc != SIGIL_OK) return rc;

    uint8_t *table = (uint8_t *)malloc(root_size);
    if (!table) return SIGIL_ERR_OOM;

    uint64_t table_off = base + (uint64_t)root_sector * SIGIL_ISO_SECTOR_SIZE;
    if (sigil_io_read_exact(io, table_off, table, root_size) != SIGIL_OK) {
        free(table);
        return SIGIL_ERR_IO;
    }

    uint32_t sector, size;
    rc = xdvdfs_lookup(table, root_size, name, &sector, &size);
    free(table);
    if (rc != SIGIL_OK) return rc;

    *out_off  = base + (uint64_t)sector * SIGIL_ISO_SECTOR_SIZE;
    *out_size = size;
    return SIGIL_OK;
}
