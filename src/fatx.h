// SPDX-License-Identifier: MPL-2.0
/* FATX, the original Xbox's file system, on partition E: of a retail-layout
 * hard drive: where the console keeps every game's saves, in
 * E:\UDATA\<title id>\ and E:\TDATA\<title id>\.
 *
 * The layout is the one in X1 BOX's xemu_fatx_import.c (izzy2lost/xemu at
 * 57bc6d6, android/app/src/main/cpp) and in libfatx (mborgerson/fatx v0.0.8:
 * fatx.c, fatx_disk.c, fatx_internal.h), which agree. E: is at byte
 * 0xABE80000 of the drive and 0x1312D6000 bytes long. Its superblock is
 * 4 KiB ("FATX", a volume id, sectors per cluster, the root directory's
 * cluster). The FAT follows, one entry per cluster of the partition plus a
 * reserved one, rounded up to 4 KiB: 16-bit entries below 0xFFF0 of them,
 * 32-bit above. 0 is a free cluster and 0xFFFFFFFF the end of a chain;
 * 0xFFFFFFF0, 0xFFFFFFF7 and 0xFFFFFFF8 mark reserved, bad and media
 * entries, which no chain runs through. Cluster 1 is the first after the
 * FAT. A directory is a chain of clusters holding 64-byte entries: the
 * name's length (0xE5 deleted, 0xFF or 0x00 the end), the attributes (0x10
 * a directory), a 42-byte name, the first cluster, the size, and three time
 * stamps packed as FAT packs them, the year counted from 2000.
 *
 * Two things rest on drives the Xbox's own kernel wrote in xemu, where
 * those sources differ from it (samples in
 * tests/fixtures/saves/xbox/manifest.tsv, read with a script written from
 * the layout above):
 *  - The stamps are creation, last write and last access, in that order;
 *    both sources name the first "modified". On orta-flatout-xemu a game
 *    rewrote the files of two save folders: each folder's second stamp
 *    moved to the time of the write, its first and third stayed.
 *  - The kernel pads a name with 0xFF and gives a new entry three equal
 *    stamps (orta-xemu, every entry); X1 BOX pads with zeros.
 *
 * Ported from CabinetOS's frontend/src/xboxhdd.cpp (MIT; the notice is in
 * licenses/cabinetos-MIT.txt). */
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
 * holds them, without those sigil_fatx_remove took out; `*out` is malloc'd.
 * SIGIL_ERR_DAMAGED when its chain leaves the volume, runs into a free
 * cluster or loops.
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

/*
 * The calls below change E:. They take clusters only from those free at
 * open, so a new file never lands where a removed one was, and stage their
 * writes on the disk in four phases (sigil_disk_set_phase), so a restore
 * written in parts that stops after any write leaves every chain whole, the
 * clusters it marked used and no entry holds as its only waste, and each
 * entry it replaces either as it was or as it was to be:
 *  0. the new files and folders, with their FAT entries, which no entry
 *     there at open points at yet;
 *  1. 0xE5 on each removed entry no new entry takes the slot of, so another
 *     spelling of a folder is gone before the new one shows;
 *  2. the entries that put the new files and folders in directories there
 *     at open, each in the slot of a removed entry of the same name when
 *     there is one, so the two never show together; the FAT entry that adds
 *     a cluster to a directory there at open;
 *  3. the FAT entries of what was removed, freed.
 * Phases 1 to 3 are staged by sigil_fatx_flush.
 */

/**
 * For a drive written back whole in one write, which no stop can show half
 * done: what sigil_fatx_remove takes out is freed at once, and its clusters
 * can be taken again, as they could before the order above was needed.
 */
void sigil_fatx_free_at_once(sigil_fatx *fs);

/**
 * Takes out a file, or a directory with everything in it: no listing shows
 * it from now on, and sigil_fatx_flush marks its entry deleted (unless a new
 * entry takes its slot) and frees its clusters, which sigil_fatx_free_at_once
 * frees here instead.
 */
int sigil_fatx_remove(sigil_fatx *fs, const sigil_fatx_entry *entry);

/**
 * A new directory `name` in directory `dir`, stamped `time` and `date`;
 * `*made` gets its cluster. SIGIL_ERR_NO_SPACE when E: or `dir` is full:
 * unless sigil_fatx_free_at_once was called, what is removed is still there
 * until the flush, so the new files need room beside it.
 */
int sigil_fatx_mkdir(sigil_fatx *fs, uint32_t dir, const char *name, uint16_t time, uint16_t date, uint32_t *made);

/** A new file `name` in directory `dir` holding `data`. SIGIL_ERR_NO_SPACE as sigil_fatx_mkdir. */
int sigil_fatx_put(sigil_fatx *fs, uint32_t dir, const char *name, const uint8_t *data, size_t len, uint16_t time,
                   uint16_t date);

/**
 * Stages the rest: the FAT entries of phase 0, then phases 1 to 3. Call
 * once, after the last change; SIGIL_ERR_INVALID_ARG for a change or a flush
 * after it.
 */
int sigil_fatx_flush(sigil_fatx *fs);

#endif
