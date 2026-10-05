// SPDX-License-Identifier: MPL-2.0
#ifndef SIGIL_INTERNAL_H
#define SIGIL_INTERNAL_H

#include "sigil.h"
#include "sigil_util.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define SIGIL_NCA_HEADER_SIZE 0xC00

static inline uint32_t sigil_read_le32(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static inline void sigil_write_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline uint64_t sigil_read_le64(const uint8_t *p) {
    return (uint64_t)sigil_read_le32(p)
         | ((uint64_t)sigil_read_le32(p + 4) << 32);
}

static inline uint32_t sigil_read_be32(const uint8_t *p) {
    return (uint32_t)p[3]
         | ((uint32_t)p[2] << 8)
         | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[0] << 24);
}

static inline uint64_t sigil_read_be64(const uint8_t *p) {
    return ((uint64_t)sigil_read_be32(p) << 32)
         |  (uint64_t)sigil_read_be32(p + 4);
}

static inline uint16_t sigil_read_le16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint16_t sigil_read_be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline void sigil_write_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void sigil_write_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void sigil_write_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static inline void sigil_write_le64(uint8_t *p, uint64_t v) {
    sigil_write_le32(p, (uint32_t)v);
    sigil_write_le32(p + 4, (uint32_t)(v >> 32));
}

static inline void sigil_write_be64(uint8_t *p, uint64_t v) {
    sigil_write_be32(p, (uint32_t)(v >> 32));
    sigil_write_be32(p + 4, (uint32_t)v);
}

/**
 * fopen for a UTF-8 `path` on every system. Windows' fopen reads the path in
 * the ANSI code page, so every file sigil opens by name goes through here.
 */
FILE *sigil_fopen(const char *path, const char *mode);

#ifdef _WIN32
#include <wchar.h>
/** `path` as UTF-16 for the Windows wide calls; NULL when it isn't UTF-8 or
 *  memory runs out. Free with free(). */
wchar_t *sigil_wide_path(const char *path);
#endif

int sigil_io_read_exact(const sigil_io *io, uint64_t off, void *buf, size_t len);

/**
 * Reads up to `len` bytes at `off`, stopping early at the end of the stream,
 * and sets `*got` to the count read. SIGIL_ERR_IO only when a read fails.
 */
int sigil_io_read_upto(const sigil_io *io, uint64_t off, void *buf, size_t len, size_t *got);

#define SIGIL_ISO_SECTOR_SIZE 2048

typedef struct {
    uint32_t lba;
    uint32_t length;
} sigil_iso_file_loc;

int sigil_iso_read_sector(const sigil_io *io, uint32_t lba, uint8_t out[SIGIL_ISO_SECTOR_SIZE]);
int sigil_iso_read_pvd_root(const sigil_io *io, uint32_t *root_lba, uint32_t *root_len);
int sigil_iso_find_file(const sigil_io *io,
                        uint32_t dir_lba, uint32_t dir_len,
                        const char *target_name,
                        sigil_iso_file_loc *out);

void sigil_aes_xts_decrypt_nintendo(const uint8_t key[32],
                                     uint64_t start_sector,
                                     uint8_t *data, size_t len);

/* Inverse of the above. Not used by extraction; exists so tests can build
 * encrypted NCA fixtures deterministically. */
void sigil_aes_xts_encrypt_nintendo(const uint8_t key[32],
                                     uint64_t start_sector,
                                     uint8_t *data, size_t len);

int sigil_decode_header_key_from_text(const char *text, size_t text_len,
                                       uint8_t out[32]);

/* Load a named 16-byte key (e.g. "key_area_key_application_00") from a
 * prod.keys file or an in-memory prod.keys blob. */
int sigil_load_key16_from_prod_keys(const char *path, const char *key_name,
                                    uint8_t out[16]);
int sigil_decode_key16_from_text(const char *text, size_t text_len,
                                 const char *key_name, uint8_t out[16]);

int sigil_resolve_header_key(const sigil_support *sup, uint8_t out[32]);

int sigil_nca_extract_title_id(const uint8_t *decrypted_header, char out_title_id[17]);

/* Tries the header as plaintext first, then decrypts a copy if a key is
 * present. SIGIL_ERR_NEEDS_KEY when plaintext fails and no key is given. */
int sigil_nca_title_from_raw_header(const uint8_t *raw_header,
                                    const uint8_t *header_key_or_null,
                                    char out_title_id[17]);

/* Plaintext-first decrypt of a raw NCA header into `out` (0xC00 bytes).
 * SIGIL_OK when a valid NCA2/NCA3 magic is present after resolving crypto,
 * SIGIL_ERR_NEEDS_KEY when encrypted and no key given, else NOT_FOUND. */
int sigil_nca_decrypt_header(const uint8_t *raw_header,
                             const uint8_t *header_key_or_null,
                             uint8_t out[SIGIL_NCA_HEADER_SIZE]);

/* AES-128-CTR (symmetric: en/decrypt). `ctr` is the initial 16-byte counter
 * and is consumed in place. Length need not be block-aligned. */
void sigil_aes_ctr_crypt(const uint8_t key[16], uint8_t ctr[16],
                         uint8_t *buf, size_t len);

/* Title facts a Switch container walk resolves. `content_type`, `version`
 * and `application_id` are populated only from CNMT; `from_cnmt` marks the
 * authoritative path. */
typedef struct {
    char     title_id[17];         /* the content's own id: an update's or a DLC's differs from its game's */
    char     application_id[17];   /* the game it belongs to, whose id the save folder carries */
    int      content_type;   /* enum sigil_switch_content_type */
    uint32_t version;
    bool     from_cnmt;
} sigil_switch_title;

/* The game a Switch content id belongs to, read from the id alone (a file
 * name's): an id ending in 800 is an update of the game ending in 000, one
 * ending in anything else but 000 a DLC of the game 0x1000 below its block. */
void sigil_switch_application_of_id(const char id[17], char out[17]);

/* Keeps `t` in `*best` when nothing is kept yet (`*have` false) or `t` is an
 * application, so a dump holding a game beside its update or DLC reads as the
 * game. True once an application is kept: the walk can stop. */
bool sigil_switch_title_keep(sigil_switch_title *best, bool *have, const sigil_switch_title *t);

/* Given a decrypted Meta NCA header plus the raw container IO and the key
 * material, decrypt the section, locate the .cnmt and read the authoritative
 * per-content title id / version / content type. */
int sigil_cnmt_from_meta_nca(const sigil_io *io, uint64_t nca_offset,
                             const uint8_t decrypted_header[SIGIL_NCA_HEADER_SIZE],
                             const sigil_support *sup,
                             sigil_switch_title *out);

/* Shared PFS0 header field parse. `hdr` is the 16-byte PFS0 header; fills the
 * entry-table / string-table / data offsets relative to the PFS0 start. */
typedef struct {
    uint32_t file_count;
    uint32_t string_table_size;
    uint64_t entries_off;
    uint64_t string_table_off;
    uint64_t data_off;
} sigil_pfs0_layout;

int sigil_pfs0_parse_header(const uint8_t hdr[16], sigil_pfs0_layout *out);

/* Walk a PFS0 partition and resolve title facts, preferring CNMT when a key
 * and prod.keys source are present, else falling back to program_id. */
int sigil_pfs0_extract_title(const sigil_io *io, uint64_t partition_off,
                             const uint8_t *header_key_or_null,
                             const sigil_support *sup_or_null,
                             sigil_switch_title *out);

void sigil_apply_switch_title(sigil_result *out, const sigil_switch_title *t);

int sigil_cnf_parse_boot(const uint8_t *cnf, size_t len,
                         const char *boot_key,
                         char raw[32], char canonical[32]);

int sigil_sfo_get_string(const uint8_t *data, size_t len,
                         const char *key,
                         char *out, size_t out_cap);

/** Where the value of `key` sits in SFO `data`: its byte offset and stated
 *  length, clipped to the data. SIGIL_ERR_NOT_FOUND when `data` isn't an SFO
 *  or lacks the key. */
int sigil_sfo_find(const uint8_t *data, size_t len, const char *key, size_t *offset, size_t *size);

#define SIGIL_POPS_SFO_SIZE 4912u

/** Signs a PSP save PARAM.SFO as the PSP save utility does: sets the
 *  SAVEDATA_PARAMS flag 0x41 and writes the hashes at 0x70 and 0x10
 *  (AES-CMAC under public KIRK key slots). The hash at 0x20 needs the
 *  console's own key; it is left as it is, and POPS doesn't check it. */
int sigil_psp_sfo_sign(uint8_t *sfo, size_t len);

/** The PARAM.SFO POPS writes in a PS1 classic's save folder, for folder
 *  `directory` and display title `title`, signed. */
int sigil_pops_param_sfo(const char *directory, const char *title, uint8_t out[SIGIL_POPS_SFO_SIZE]);

int sigil_extract_psp(const sigil_io *io, const char *filename_hint,
                      const sigil_options *opts, sigil_result *out);
int sigil_extract_psx(const sigil_io *io, const char *filename_hint,
                      const sigil_options *opts, sigil_result *out);
int sigil_extract_ps2(const sigil_io *io, const char *filename_hint,
                      const sigil_options *opts, sigil_result *out);
int sigil_extract_ps3(const sigil_io *io, const char *filename_hint,
                      const sigil_options *opts, sigil_result *out);
/* The memory-card folder stem a PS2 serial produces, region prefix included.
 * Shared with the filename fallback so a serial read off the name lands on the
 * same on-disk stem as one read off SYSTEM.CNF. */
void sigil_ps2_save_id_stem(const char title_id[32], char out_save_id[32]);
int sigil_extract_xbox360(const sigil_io *io, const char *filename_hint,
                          const sigil_options *opts, sigil_result *out);
/* Wii and GameCube share every container and all but one extension, so one
 * reader serves both. `platform` is the console the caller named; AUTO means
 * nobody named one and the disc header magic has to answer. */
int sigil_extract_nintendo_disc(const sigil_io *io, sigil_platform platform,
                                const sigil_options *opts, sigil_result *out);
int sigil_extract_3ds(const sigil_io *io, const char *filename_hint,
                      const sigil_options *opts, sigil_result *out);
int sigil_extract_switch(const sigil_io *io, const char *filename_hint,
                         const sigil_options *opts, sigil_result *out);
int sigil_extract_wiiu(const sigil_io *io, const char *filename_hint,
                       const sigil_options *opts, sigil_result *out);
int sigil_extract_psvita(const sigil_io *io, const char *filename_hint,
                         const sigil_options *opts, sigil_result *out);
int sigil_extract_dreamcast(const sigil_io *io, const char *filename_hint,
                            const sigil_options *opts, sigil_result *out);
int sigil_extract_xbox(const sigil_io *io, const char *filename_hint,
                       const sigil_options *opts, sigil_result *out);
int sigil_extract_gb(const sigil_io *io, const char *filename_hint,
                     const sigil_options *opts, sigil_result *out);
int sigil_extract_snes(const sigil_io *io, const char *filename_hint,
                       const sigil_options *opts, sigil_result *out);
int sigil_extract_n64(const sigil_io *io, const char *filename_hint,
                      const sigil_options *opts, sigil_result *out);

typedef struct {
    uint32_t state[4];
    uint64_t length;
    uint8_t  buffer[64];
    size_t   buffered;
} sigil_md5;

void sigil_md5_init(sigil_md5 *m);
void sigil_md5_update(sigil_md5 *m, const void *data, size_t len);
void sigil_md5_final(sigil_md5 *m, uint8_t digest[16]);
void sigil_md5_hex(const uint8_t digest[16], char out[33]);

/** The MD5 of `len` bytes, as lowercase hex. */
void sigil_md5_of(const void *data, size_t len, char out[33]);

/* SHA-1 (FIPS 180-4) and HMAC-SHA1 (RFC 2104), for the PSP and Vita PS1
 * card signature. */
typedef struct {
    uint32_t state[5];
    uint64_t length;
    uint8_t  buffer[64];
    size_t   buffered;
} sigil_sha1;

void sigil_sha1_init(sigil_sha1 *s);
void sigil_sha1_update(sigil_sha1 *s, const void *data, size_t len);
void sigil_sha1_final(sigil_sha1 *s, uint8_t digest[20]);

/** HMAC-SHA1 of `len` bytes of `data` under `key`. */
void sigil_hmac_sha1(const uint8_t *key, size_t key_len, const void *data, size_t len, uint8_t mac[20]);

/** One named part of a unit and the MD5 of its bytes. */
typedef struct {
    char name[SIGIL_SAVE_PATH_MAX];
    char md5[33];
} sigil_named_md5;

/**
 * RomM's hash over named parts, as it hashes a zip: the MD5 of the lines
 * "<name>:<md5>", sorted by name in byte order and joined with "\n". Sorts
 * `items` in place.
 */
void sigil_named_hash(sigil_named_md5 *items, size_t count, char out[33]);

/**
 * The shared files the request's layout row applies, expanded, whether or
 * not the listing holds them, with each one's sigil_device in `devices` when
 * given. Files of one device other than SIGIL_DEVICE_NONE are alternatives:
 * the emulator uses one of them. Returns how many went into `out`.
 */
size_t sigil_save_shared_paths(const sigil_save_request *req, char (*out)[SIGIL_SAVE_PATH_MAX], int *devices,
                               size_t cap);

/** The request's value for option `key`, or NULL when it gives none. */
const char *sigil_save_option_value(const sigil_save_request *req, const char *key);

/** `path` is in the request's listing. */
bool sigil_save_listed(const sigil_save_request *req, const char *path);

/**
 * The listed files the layout would take under other option values, each with
 * the values that take it; `*out` is freed with free(). None on a layout with
 * profiles. SIGIL_ERR_OOM is the only failure.
 */
int sigil_save_alternates(const sigil_save_request *req, sigil_save_alternate **out, size_t *count);

/**
 * The region letter that ends a GameCube game code, from the result's raw
 * serial or else its hex title id; 0 when neither holds one.
 */
char sigil_gc_region_letter(const sigil_result *result);

/**
 * Dolphin's region folder for a GameCube region letter: E is USA, J (and K,
 * which Dolphin files with Japan) is JAP, every other letter is a PAL release
 * under EUR. Dolphin itself reads the disc's region field, which follows the
 * letter.
 */
const char *sigil_gc_region_folder(char letter);

/**
 * One row of the save-name table: the start of the names a product writes
 * to backup RAM or a VMU, for platforms whose saves carry no game id.
 */
typedef struct {
    const char *platform;   /* "saturn", "segacd", "dreamcast" */
    const char *code;       /* product code as the disc header spells it, trimmed */
    const char *prefix;     /* every save name the product writes starts with this */
} sigil_save_name_row;

extern const sigil_save_name_row sigil_save_name_table[];
extern const size_t sigil_save_name_table_count;

/**
 * True when `name` starts with a prefix `rows` give one of `ids` on
 * `platform`, and with none that rows give another product: a name two
 * products share can't be told apart and doesn't match. An id matches a
 * row's code ignoring spaces, a leading Sega CD type ("GM ", "AI ") and a
 * trailing version ("-00").
 */
bool sigil_save_names_match(const sigil_save_name_row *rows, size_t count, const char *platform, const char *name,
                            const char *const *ids, size_t id_count);

/** One file of a zip held in memory. */
typedef struct {
    char     name[SIGIL_SAVE_ENTRY_MAX];
    uint8_t *data;
    size_t   len;
} sigil_zip_member;

/**
 * A zip of `members` in the given order, stored uncompressed with fixed
 * times, so the same members always give the same bytes. `*out` is malloc'd.
 */
int sigil_zip_store(const sigil_zip_member *members, size_t count, uint8_t **out, size_t *len);

/**
 * Reads every file of the zip `zip`, stored or deflated, into `*out`, which
 * the caller frees with sigil_zip_members_free. Folder entries are skipped.
 * SIGIL_ERR_UNSUPPORTED_FORMAT when it isn't a zip, a member is encrypted,
 * fails its CRC or is larger than `max_member` bytes.
 */
int sigil_zip_read_mem(const uint8_t *zip, size_t len, size_t max_member, sigil_zip_member **out, size_t *count);
void sigil_zip_members_free(sigil_zip_member *members, size_t count);

/** The one file holding a backup RAM device for the request's game. */
typedef struct {
    char path[SIGIL_SAVE_PATH_MAX];
    int      device;     /* sigil_device in save_layout.h */
    bool     per_game;   /* a file only this game uses, as opposed to one the core shares */
    int      form;       /* sigil_volume_form of a file the core creates */
    uint32_t new_size;   /* collapsed bytes of a file the core creates; 0 for the format's default */
} sigil_volume_target;

#define SIGIL_VOLUME_TARGETS_MAX 10

/**
 * The volume file for each device the request's layout row gives the game,
 * in device order; `*count` is 0 for a row without volumes. Of several
 * members for one device, the first present wins, else the first. A shared
 * file the core picks by the disc's region comes from the row's region
 * option when forced, else from the content file name's region tag, else
 * from the only such file present. SIGIL_ERR_NOT_FOUND when none of those
 * settles it.
 */
int sigil_save_volume_targets(const sigil_save_request *req, sigil_volume_target out[SIGIL_VOLUME_TARGETS_MAX],
                              size_t *count);

/* Feeds every file entry of a zip presented as a stream to `on_entry`, in
 * central-directory order. The callback receives the entry name and the md5
 * hex of its uncompressed bytes. SIGIL_ERR_UNSUPPORTED_FORMAT when the stream
 * is not a zip. */
int sigil_zip_hash_entries(const sigil_io *io,
                           int (*on_entry)(void *ctx, const char *name, const char *md5_hex),
                           void *ctx);
bool sigil_io_is_zip(const sigil_io *io);

/* Locates `name` in the root directory of an XDVDFS image, probing the known
 * partition bases so trimmed and full disc images both resolve. Shared by both
 * Xbox generations, which use the same filesystem. */
int sigil_xdvdfs_find_root_file(const sigil_io *io, const char *name,
                                uint64_t *out_off, uint32_t *out_size);

/* ZArchive is one container behind two extensions: Cemu's .wua for Wii U and
 * Xenia's .zar for Xbox 360. The declarations stay unconditional so either
 * platform can be built without the other. */
#define SIGIL_ZAR_MAGIC        0x169F52D6u
#define SIGIL_ZAR_VERSION_1    0x61BF3A01u
#define SIGIL_ZAR_FOOTER_SIZE  144
#define SIGIL_ZAR_TREE_ENTRY   16
/* Contents are stored in fixed 64 KiB blocks with an offset record every 16,
 * which is what makes random access into a compressed archive cheap. */
#define SIGIL_ZAR_BLOCK_SIZE   (64u * 1024u)
#define SIGIL_ZAR_BLOCKS_PER_RECORD 16
#define SIGIL_ZAR_RECORD_SIZE  (8 + 2 * SIGIL_ZAR_BLOCKS_PER_RECORD)
#define SIGIL_ZAR_MAX_METADATA (16 * 1024 * 1024)

typedef struct {
    uint64_t compressed_off,     compressed_size;
    uint64_t offset_records_off, offset_records_size;
    uint64_t names_off,          names_size;
    uint64_t file_tree_off,      file_tree_size;
} sigil_zar_footer;

int sigil_zar_read_footer(const sigil_io *io, sigil_zar_footer *out);
size_t sigil_zar_read_name(const uint8_t *names, size_t names_len,
                           uint32_t offset, char *out, size_t out_size);

int sigil_filename_fallback(const char *filename_hint,
                            sigil_platform platform,
                            sigil_result *out);

static inline void sigil_result_init(sigil_result *r) {
    memset(r, 0, sizeof(*r));
    r->struct_version = SIGIL_RESULT_V4;
}

#endif
