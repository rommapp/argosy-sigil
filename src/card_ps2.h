// SPDX-License-Identifier: MPL-2.0
#ifndef SIGIL_CARD_PS2_H
#define SIGIL_CARD_PS2_H

#include "card.h"

#define PS2_IFC_SLOTS 32

/** A PS2 card with its pages joined into clusters and the ECC set aside. */
typedef struct {
    uint8_t  *clusters;           /* clusters_per_card * cluster_size bytes */
    uint32_t  cluster_size;
    uint32_t  clusters_per_card;
    uint32_t  alloc_offset;       /* absolute cluster where allocatable clusters start */
    uint32_t  alloc_end;          /* allocatable clusters, counted from alloc_offset */
    uint32_t  rootdir_cluster;    /* relative to alloc_offset */
    uint32_t  ifc[PS2_IFC_SLOTS]; /* absolute clusters of the indirect FAT */
    int       ecc;                /* 1 when the file carried 16 spare bytes per page */
    uint8_t  *spare;              /* 16 spare bytes per page as loaded; NULL without ECC. Kept as
                                     read, since cards carry erased (0xFF) and stale spares */
    uint8_t  *dirty;              /* one flag per page whose data sigil changed; its spare is
                                     recomputed on write */
} sigil_ps2_card;

/** Loads and lists a PS2 .ps2 file card; SIGIL_ERR_UNSUPPORTED_FORMAT when it isn't one. */
int sigil_ps2_card_list_io(const sigil_io *io, sigil_card_listing **out);

/** Reads a .ps2 file card. Free with sigil_ps2_card_free. */
int sigil_ps2_card_load(const sigil_io *io, sigil_ps2_card *card);
void sigil_ps2_card_free(sigil_ps2_card *card);

/** Lists the save folders in the card's root directory. */
int sigil_ps2_card_list(const sigil_ps2_card *card, sigil_card_listing **out);

/**
 * The files of the save folder starting at cluster `first_cluster` (relative),
 * joined in directory order, each at its stated length. `*out` is malloc'd.
 */
int sigil_ps2_save_data(const sigil_ps2_card *card, uint32_t first_cluster,
                        uint8_t **out, size_t *len);

#define PS2_TOD_SIZE 8

/**
 * Makes `card` an empty formatted 8 MB card with ECC, laid out as PCSX2 and
 * mymc lay one out. `tod` is the 8-byte card timestamp stamped on the root
 * directory (unused, second, minute, hour, day, month, year LE16).
 */
int sigil_ps2_card_format(sigil_ps2_card *card, const uint8_t tod[PS2_TOD_SIZE]);

/** Bytes the card takes as a .ps2 file. */
size_t sigil_ps2_card_file_size(const sigil_ps2_card *card);

/** Writes the card as a .ps2 file into `out`, which holds sigil_ps2_card_file_size bytes. */
int sigil_ps2_card_write(const sigil_ps2_card *card, uint8_t *out);

/** The 3-byte ECC PS2 cards keep for each 128-byte chunk of a page. */
void sigil_ps2_ecc(const uint8_t chunk[128], uint8_t ecc[3]);

#define PS2_DIR_ENTRY_SIZE 512

/** One file of a save folder: its 512-byte directory entry and its bytes. */
typedef struct {
    uint8_t  entry[PS2_DIR_ENTRY_SIZE];
    uint8_t *data;                     /* the entry's stated length */
} sigil_ps2_file;

/**
 * A save folder lifted off a card: the root's entry for it, its own "." and
 * ".." entries (they carry the folder's timestamps), and its files in
 * directory order.
 */
typedef struct {
    uint8_t         entry[PS2_DIR_ENTRY_SIZE];
    uint8_t         self[PS2_DIR_ENTRY_SIZE];
    uint8_t         parent[PS2_DIR_ENTRY_SIZE];
    sigil_ps2_file *files;
    size_t          file_count;
} sigil_ps2_save;

void sigil_ps2_save_free(sigil_ps2_save *save);

/**
 * Lifts the save folder starting at relative cluster `first_cluster` off the
 * card. SIGIL_ERR_UNSUPPORTED_FORMAT when it doesn't read, or holds a folder
 * of its own.
 */
int sigil_ps2_extract(const sigil_ps2_card *card, uint32_t first_cluster, sigil_ps2_save *out);

/**
 * Writes `save` into the card's root, allocating the lowest free clusters in
 * the order the PS2 writes them: the root's next cluster when it needs one,
 * the folder's first cluster, then for each file its entry's cluster when
 * needed and its data. SIGIL_ERR_NO_SPACE when the card lacks room;
 * SIGIL_ERR_EXISTS when a folder with the same name exists. Either way
 * the card is left unchanged.
 */
int sigil_ps2_inject(sigil_ps2_card *card, const sigil_ps2_save *save);

/** Deletes the save folder starting at `first_cluster`, freeing its clusters. */
int sigil_ps2_delete(sigil_ps2_card *card, uint32_t first_cluster);

/**
 * SIGIL_OK when the card holds a folder named as in `save` whose entries and
 * files equal it; SIGIL_ERR_NOT_FOUND otherwise.
 */
int sigil_ps2_verify(const sigil_ps2_card *card, const sigil_ps2_save *save);

/**
 * The MD5 over the folder's name and each file's name, length and bytes in
 * directory order. Timestamps, modes and cluster placement leave it alone.
 */
void sigil_ps2_save_md5(const sigil_ps2_save *save, char out[33]);

#define PS2_FOLDER_SUPERBLOCK_SIZE 0x2000u

/**
 * The `_pcsx2_superblock` of an empty 8 MB folder card: the superblock page
 * sigil_ps2_card_format writes, then zeros to the full erase block PCSX2
 * reads.
 */
void sigil_ps2_folder_superblock(uint8_t out[PS2_FOLDER_SUPERBLOCK_SIZE]);

/**
 * True when PCSX2 reads `data` as a formatted folder card's superblock: all
 * 0x2000 bytes present and byte 0x16 the 'o' of "Format". PCSX2 shows no
 * saves on a card whose superblock is missing, empty or short, so a restore
 * writes one when this is false.
 */
bool sigil_ps2_folder_superblock_usable(const uint8_t *data, size_t len);

#define PS2_FOLDER_PATH_MAX 96

/** One file of a PCSX2 folder-card save folder, by its path inside the folder. */
typedef struct {
    char     path[PS2_FOLDER_PATH_MAX];  /* "icon.sys", "_pcsx2_index", "_pcsx2_meta/icon.sys" */
    uint8_t *data;
    size_t   len;
} sigil_ps2_folder_file;

void sigil_ps2_folder_files_free(sigil_ps2_folder_file *files, size_t count);

/**
 * Writes `save` as the files of a PCSX2 folder-card save folder: its files,
 * an _pcsx2_index carrying each file's order and times and the folder's own
 * times, and _pcsx2_meta entries for any entry PCSX2 couldn't rebuild from
 * the index alone. Card dates convert to Unix seconds as UTC, as PCSX2
 * converts them. `*files` is malloc'd.
 */
int sigil_ps2_unpack(const sigil_ps2_save *save, sigil_ps2_folder_file **files, size_t *count);

/**
 * Reads the files of a PCSX2 folder-card save folder named `folder` into a
 * save, the way PCSX2 builds its card: entries in the index's order, files
 * the index doesn't name first in the order given, times from the index,
 * and _pcsx2_meta entries taking precedence. Reads the index in block or flow
 * YAML. SIGIL_ERR_UNSUPPORTED_FORMAT when the index doesn't parse.
 */
int sigil_ps2_pack(const char *folder, const sigil_ps2_folder_file *files, size_t count, sigil_ps2_save *out);

#endif
