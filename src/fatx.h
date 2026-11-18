// SPDX-License-Identifier: MPL-2.0
/* FATX, the original Xbox's file system, on partition E: of a retail-layout
 * hard drive: where the console keeps every game's saves, in
 * E:\UDATA\<title id>\ and E:\TDATA\<title id>\.
 *
 * The partition sits at a fixed place on the drive. Its superblock is 4 KiB
 * ("FATX", a volume id, sectors per cluster, the root directory's cluster).
 * The FAT follows, one entry per cluster plus a reserved one, rounded up to
 * 4 KiB: 16-bit entries below 0xFFF0 clusters, 32-bit above, 0 for a free
 * cluster and 0xFFFFFFF0 or more for the end of a chain. Cluster 1 is the
 * first after the FAT. A directory is a chain of clusters holding 64-byte
 * entries: the name's length (0xE5 deleted, 0xFF or 0x00 the end), the
 * attributes, a 42-byte name padded with 0xFF, the first cluster, the size,
 * and three time stamps packed as FAT packs them, the year counted from 2000.
 *
 * Ported from CabinetOS's frontend/src/xboxhdd.cpp (MIT; the notice is in
 * licenses/cabinetos-MIT.txt). The layout was checked there against xemu's
 * published drive and against drives the Xbox's own kernel formatted and
 * wrote in xemu. */
#ifndef SIGIL_FATX_H
#define SIGIL_FATX_H

#include "disk_image.h"

#define SIGIL_FATX_NAME_MAX 42u
#define SIGIL_FATX_ATTR_DIR 0x10u

typedef struct sigil_fatx sigil_fatx;

typedef struct {
    char     name[SIGIL_FATX_NAME_MAX + 1];
    uint8_t  attr;
    uint32_t first;   /* its first cluster; 0 for an empty file */
    uint32_t size;    /* a file's bytes; 0 for a directory */
    uint16_t time;    /* its last write, as FATX packs it: hours << 11 | minutes << 5 | seconds / 2 */
    uint16_t date;    /* (year - 2000) << 9 | month << 5 | day */
    uint64_t at;      /* where its 64 bytes are on the drive */
} sigil_fatx_entry;

/**
 * Opens E: on `d`, which stays the caller's. SIGIL_ERR_DAMAGED when the
 * drive is too small for a retail layout or E: isn't a FATX volume.
 */
int  sigil_fatx_open(sigil_disk *d, sigil_fatx **out);
void sigil_fatx_close(sigil_fatx *fs);

uint32_t sigil_fatx_root(const sigil_fatx *fs);
uint32_t sigil_fatx_cluster_size(const sigil_fatx *fs);
uint32_t sigil_fatx_free_clusters(const sigil_fatx *fs);

/**
 * The entries of the directory starting at cluster `dir`, in the order it
 * holds them; `*out` is malloc'd. SIGIL_ERR_DAMAGED when its chain leaves the
 * volume, runs into a free cluster or loops.
 */
int sigil_fatx_list(sigil_fatx *fs, uint32_t dir, sigil_fatx_entry **out, size_t *count);

/** The entry named `name` in directory `dir`, compared without case; SIGIL_ERR_NOT_FOUND when there is none. */
int sigil_fatx_find(sigil_fatx *fs, uint32_t dir, const char *name, sigil_fatx_entry *out);

/** A file's bytes; `*out` is malloc'd. SIGIL_ERR_DAMAGED when its chain is shorter than its size. */
int sigil_fatx_read(sigil_fatx *fs, const sigil_fatx_entry *file, uint8_t **out, size_t *len);

/** `name` can be a FATX name: 1 to 42 characters, none a control character,
 *  a quote, an asterisk, a slash or backslash, a colon, an angle bracket, a
 *  question mark or a bar. */
bool sigil_fatx_name_ok(const char *name);

/**
 * Removes a file, or a directory with everything in it: its clusters are
 * freed and its entry marked deleted. Staged on the disk, like every change.
 */
int sigil_fatx_remove(sigil_fatx *fs, const sigil_fatx_entry *entry);

/**
 * A new directory `name` in directory `dir`, stamped `time` and `date`;
 * `*made` gets its cluster. SIGIL_ERR_NO_SPACE when E: or `dir` is full.
 */
int sigil_fatx_mkdir(sigil_fatx *fs, uint32_t dir, const char *name, uint16_t time, uint16_t date, uint32_t *made);

/** A new file `name` in directory `dir` holding `data`. SIGIL_ERR_NO_SPACE when E: or `dir` is full. */
int sigil_fatx_put(sigil_fatx *fs, uint32_t dir, const char *name, const uint8_t *data, size_t len, uint16_t time,
                   uint16_t date);

/** Stages the FAT entries the calls above changed. Call once, after the last of them. */
int sigil_fatx_flush(sigil_fatx *fs);

#endif
