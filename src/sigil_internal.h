// SPDX-License-Identifier: MPL-2.0
#ifndef SIGIL_INTERNAL_H
#define SIGIL_INTERNAL_H

#include "sigil.h"
#include "sigil_util.h"
#include <stdbool.h>
#include <string.h>

#define SIGIL_NCA_HEADER_SIZE 0xC00

static inline uint32_t sigil_read_le32(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
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

int sigil_io_read_exact(const sigil_io *io, uint64_t off, void *buf, size_t len);

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

/* Title facts a Switch container walk resolves. `content_type` and `version`
 * are populated only from CNMT; `from_cnmt` marks the authoritative path. */
typedef struct {
    char     title_id[17];
    int      content_type;   /* enum sigil_switch_content_type */
    uint32_t version;
    bool     from_cnmt;
} sigil_switch_title;

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

int sigil_extract_psp(const sigil_io *io, const char *filename_hint,
                      const sigil_options *opts, sigil_result *out);
int sigil_extract_psx(const sigil_io *io, const char *filename_hint,
                      const sigil_options *opts, sigil_result *out);
int sigil_extract_ps2(const sigil_io *io, const char *filename_hint,
                      const sigil_options *opts, sigil_result *out);
int sigil_extract_ps3(const sigil_io *io, const char *filename_hint,
                      const sigil_options *opts, sigil_result *out);
int sigil_extract_xbox360(const sigil_io *io, const char *filename_hint,
                          const sigil_options *opts, sigil_result *out);
int sigil_extract_wii(const sigil_io *io, const char *filename_hint,
                      const sigil_options *opts, sigil_result *out);
int sigil_extract_gamecube(const sigil_io *io, const char *filename_hint,
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
    r->struct_version = SIGIL_RESULT_V3;
}

#endif
