// SPDX-License-Identifier: MPL-2.0
/* Shared by the sync sources: sync.c (collect and restore), sync_state.c,
 * the card kinds in sync_kind_*.c, sync_units.c, and the placement paths in
 * sync_cards.c, sync_folder_cards.c, sync_volumes.c, sync_gci_folder.c and
 * sync_profiles.c. */
#ifndef SIGIL_SYNC_INTERNAL_H
#define SIGIL_SYNC_INTERNAL_H

#include "card_dreamcast.h"
#include "card_gamecube.h"
#include "card_ps1.h"
#include "card_ps2.h"
#include "card_segacd.h"
#include "save_profiles.h"
#include <stdio.h>
#include <stdlib.h>

#define SYNC_KEY_MAX        (4 * SIGIL_SAVE_PATH_MAX)
#define SYNC_MAX_CARD_FILES (8)
#define SYNC_MAX_SHARED     (8)
#define SYNC_MAX_UNIT_MEMBER (8u * 1024u * 1024u + 64u * 1024u)

/* ---- state (sync_state.c) ---------------------------------------------------- */

typedef struct {
    char  **lines;
    size_t  count;
    size_t  cap;
} sigil_sync_state;

int  sigil_sync_state_parse(sigil_sync_state *s, const uint8_t *blob, size_t len);
void sigil_sync_state_free(sigil_sync_state *s);
int  sigil_sync_state_blob(const sigil_sync_state *s, uint8_t **out, size_t *len);
/** The value of `tag` for `key`, or NULL. */
const char *sigil_sync_state_get(const sigil_sync_state *s, const char *tag, const char *key);
/** Sets `tag` for `key` to `value`; drops it when `value` is NULL. */
int sigil_sync_state_put(sigil_sync_state *s, const char *tag, const char *key, const char *value);
/** The game a save named `name` on volume `volume` belongs to, or NULL. */
const char *sigil_sync_owner_get(const sigil_sync_state *s, const char *volume, const char *name);
int sigil_sync_owner_put(sigil_sync_state *s, const char *volume, const char *name, const char *game);
/** `in` with '%', tab, CR and newline written as %XX, as state fields need. */
void sigil_sync_escape(const char *in, char *out, size_t cap);

/* ---- card kinds (sync_kind_*.c) ---------------------------------------------- */

/* What sync needs from a card or volume format. `card` and `save` are the
 * format's own types; sync only moves them between these calls. */
typedef struct {
    const char *platform;     /* the slug the state keys this platform's games under */
    bool        has_ids;      /* saves carry their game's id; else ownership comes from volumes */
    int         main_device;  /* the volume a unit may carry alone, unzipped; SIGIL_DEVICE_NONE on cards */
    /* Reads a card or volume holding `device` (SIGIL_DEVICE_NONE on card platforms). */
    int    (*load)(const sigil_io *io, int device, void **card, int *format);
    /* An empty card or volume for `device`: the size and stored form of `like`
     * when given (a VMU also keeps its layout), else `size` collapsed bytes
     * stored in `form`. A kind with one card size gives that card whatever
     * `size` says. */
    int    (*blank)(void **card, int *format, int device, size_t size, int form, const void *like);
    void   (*free_card)(void *card);
    size_t (*size)(const void *card);
    size_t (*unit_size)(int device, const void *source);
    int    (*list)(const void *card, int format, sigil_card_listing **out);
    int    (*extract)(const void *card, const sigil_card_entry *entry, void **save);
    void   (*free_save)(void *save);
    int    (*inject)(void *card, const void *save);
    /* The blocks inject takes for `save` on `card`, in the unit its listing
     * counts free_blocks in, a directory block it adds included. */
    uint32_t (*cost)(const void *card, const void *save);
    int    (*remove)(void *card, const sigil_card_entry *entry);
    int    (*verify)(const void *card, const void *save);
    int    (*image)(const void *card, uint8_t **out, size_t *len);
    int    (*identity)(const void *save, char out[33]);
    /* Optional. SIGIL_ERR_DAMAGED when the card as read is damaged in a way
     * writing it again repairs (a .vmp whose signature doesn't match). */
    int    (*check)(const void *card);
    /* Optional. The name a save is known by in units and the state, when the
     * card's name alone doesn't identify it (GameCube: game, maker, file). */
    void   (*save_key)(const void *save, char out[SIGIL_CARD_NAME_MAX]);
    /* Optional. The form a new card at `path` for this game is formatted in. */
    int    (*new_form)(const sigil_sync_request *req, const char *path);
    /* Optional. A unit is a raw card: when the game's card file is in another
     * form (a .vmp), the unit is named <stem> with this extension instead. */
    const char *raw_ext;
    /* Optional. Writes the files the emulator needs beside card file `path`
     * after restore wrote it, when they aren't there (POPS's PARAM.SFO). */
    int    (*beside)(const sigil_sync_request *req, const char *path, const void *card);
    /* The unit is the game's saves as files instead of a card: each travels
     * under file_name and reads back through file_to_save. */
    bool        save_files;
    void   (*file_name)(const void *save, char *out, size_t cap);
    int    (*file_to_save)(const uint8_t *data, size_t len, void **save);
    /* Optional. The save can't go beside this game's, as a companion. */
    bool   (*foreign)(const sigil_sync_request *req, const void *save);
    /* A shared card may be a PCSX2 folder card. */
    bool        folder_cards;
} sigil_sync_kind;

extern const sigil_sync_kind sigil_sync_ps1_kind;
extern const sigil_sync_kind sigil_sync_ps2_kind;
extern const sigil_sync_kind sigil_sync_saturn_kind;
extern const sigil_sync_kind sigil_sync_segacd_kind;
extern const sigil_sync_kind sigil_sync_vmu_kind;
extern const sigil_sync_kind sigil_sync_gamecube_kind;

/** The kind for the request's platform, or NULL when sync doesn't cover it. */
const sigil_sync_kind *sigil_sync_kind_for(const sigil_sync_request *req);

/* A save as bytes: .mcs, .BUP, .scd, .dci, .gci. */
typedef struct {
    uint8_t *data;
    size_t   len;
} sigil_sync_blob;

/** Takes ownership of `data`; frees it and returns NULL when out of memory. */
sigil_sync_blob *sigil_sync_blob_new(uint8_t *data, size_t len);
void sigil_sync_blob_free(void *save);
/** Copies a fixed-size raw card image into a malloc'd buffer. */
int sigil_sync_copy_image(const uint8_t *card, size_t size, uint8_t **out, size_t *len);
size_t sigil_sync_no_size(const void *card);
size_t sigil_sync_no_unit_size(int device, const void *source);

/* ---- request and saves (sync_units.c) ---------------------------------------- */

typedef struct {
    const sigil_sync_request *req;
    const sigil_sync_kind    *kind;
    char                      game[SYNC_KEY_MAX];              /* escaped state key of this game */
    char                    (*companion_keys)[SYNC_KEY_MAX];   /* escaped state key of each request companion */
    sigil_sync_state          state;
} sigil_sync_ctx;

int  sigil_sync_ctx_open(sigil_sync_ctx *x, const sigil_sync_request *req, const sigil_sync_kind *kind);
void sigil_sync_ctx_close(sigil_sync_ctx *x);

bool sigil_sync_owned_by_game(const sigil_sync_request *req, const char *owner);
/** The request companion whose ids include `owner`, or SIZE_MAX. */
size_t sigil_sync_companion_of(const sigil_sync_request *req, const char *owner);
/** The request companion whose state key is `key`, or SIZE_MAX. */
size_t sigil_sync_companion_by_key(const sigil_sync_ctx *x, const char *key);
/** The restore replaces companion `c`'s saves: the request carries its unit. */
bool sigil_sync_companion_restored(const sigil_sync_ctx *x, size_t c);
bool sigil_sync_claimed(const sigil_sync_request *req, const char *name);
/** `path` is in the request's listing. */
bool sigil_sync_listed(const sigil_sync_request *req, const char *path);
/**
 * Reads all of `path` through the request's open callback, up to `cap` bytes.
 * SIGIL_ERR_NOT_FOUND when it won't open and isn't listed; SIGIL_ERR_IO when
 * it is listed and won't open, since a file the listing shows is there.
 */
int sigil_sync_read_file(const sigil_sync_request *req, const char *path, size_t cap, uint8_t **out, size_t *len);
/** `path` holds exactly `len` bytes of `data`. */
bool sigil_sync_file_holds(const sigil_sync_request *req, const char *path, const uint8_t *data, size_t len);
/** `path` lies under the save root: relative, with no ".." segment. */
bool sigil_sync_path_inside(const char *path);
/** Writes `path` through the request and reads it back; SIGIL_ERR_IO when
 *  either fails or `path` would leave the save root. */
int sigil_sync_put(const sigil_sync_request *req, const char *path, const uint8_t *data, size_t len);
/** Removes `path` through the request; SIGIL_ERR_IO when that fails or
 *  `path` would leave the save root. */
int sigil_sync_drop(const sigil_sync_request *req, const char *path);

enum { SYNC_OWN_GAME, SYNC_OWN_OTHER, SYNC_OWN_NONE, SYNC_OWN_COMPANION };

/* Whose saves a unit or card read takes: the game's, the game's and its
 * companions', or one companion's (its index). */
#define SYNC_WHO_GAME  SIZE_MAX
#define SYNC_WHO_LOCAL (SIZE_MAX - 1)

typedef struct {
    char   name[SIGIL_CARD_NAME_MAX];
    int    device;
    void  *save;
    size_t card;                       /* index into the card or volume set it came from, or goes to */
    int    owner;
    size_t companion;                  /* the request companion when owner is SYNC_OWN_COMPANION */
    char   other[SYNC_KEY_MAX];        /* the owning game's key when owner is SYNC_OWN_OTHER or _COMPANION */
    char   path[SIGIL_SAVE_PATH_MAX];  /* the file a save in a save folder came from */
} sigil_sync_save;

typedef struct {
    const sigil_sync_kind *kind;
    sigil_sync_save       *items;
    size_t                 count;
    size_t                 cap;
} sigil_sync_saves;

void sigil_sync_saves_init(sigil_sync_saves *l, const sigil_sync_kind *kind);
void sigil_sync_saves_free(sigil_sync_saves *l);
bool sigil_sync_saves_has(const sigil_sync_saves *l, const char *name, int device);
/** A zeroed slot at the end of `l`; count it with l->count++ once filled. NULL when out of memory. */
sigil_sync_save *sigil_sync_saves_push(sigil_sync_saves *l);
/** Marks `o` as companion `c`'s. */
void sigil_sync_set_companion(const sigil_sync_ctx *x, sigil_sync_save *o, size_t c);
size_t sigil_sync_count_where(const sigil_sync_saves *l, int owner, size_t card);

/**
 * The hash over each save's name and data, so timestamps and block placement
 * don't read as a new save. Only saves whose owner is `owner` (all when
 * negative) count; with `other`, only those of that game; `device` limits it
 * to one device when not negative; `card` to one card when not SIZE_MAX.
 */
int sigil_sync_identity_where(const sigil_sync_saves *l, int owner, const char *other, int device, size_t card,
                              char out[33]);
/** The hash over the game's own saves, companions' left out. */
int sigil_sync_identity_of(const sigil_sync_saves *l, char out[33]);
/** As sigil_sync_identity_where for `owner` (and companion `key`), or "" when there are none. */
int sigil_sync_identity_or_empty(const sigil_sync_saves *l, int owner, const char *key, char out[33]);

/** The name each device's volume travels under in a unit; "" for none. */
const char *sigil_sync_device_name(int device);
int sigil_sync_load_bytes(const sigil_sync_kind *kind, const uint8_t *data, size_t len, int device, void **card,
                          int *format);

/** Adds the saves on `card` that `who` takes to `out`, each noted with `device` and card `index`. */
int sigil_sync_add_card_saves(const sigil_sync_ctx *x, const void *card, int format, int device, size_t index,
                              size_t who, sigil_sync_saves *out);

typedef struct {
    const void *source[SIGIL_DEVICE_COUNT];   /* the card each device's saves came from, by sigil_device */
} sigil_sync_sources;

/** The unit for the saves in `saves` with `owner`; *out is NULL when there are none. */
int sigil_sync_build_unit(const sigil_sync_saves *saves, int owner, const sigil_sync_sources *src, bool holding,
                          const char *stem, uint8_t **out, size_t *len, int *shape,
                          char artifact[SIGIL_SAVE_ENTRY_MAX], char content[33]);
/**
 * The game's unit, then each companion's unit given, into `out`; `sizes` gets each device's volume size.
 * SIGIL_ERR_REGION, naming the save in r->problem, for a companion's save the game can't read.
 */
int sigil_sync_request_saves(const sigil_sync_ctx *x, const uint8_t *unit, size_t len, sigil_sync_saves *out,
                             size_t sizes[SIGIL_DEVICE_COUNT], sigil_sync_result *r);

/** Names the save that didn't fit on `card` in r->problem and the blocks it lacked in r->blocks_short. */
void sigil_sync_note_overflow(const sigil_sync_ctx *x, const void *card, int format, const sigil_sync_save *o,
                              sigil_sync_result *r);
/** Writes `card` to `path`, reads it back and verifies every save in `placed` on card `index`. */
int sigil_sync_write_and_verify(const sigil_sync_ctx *x, const char *path, int device, const void *card,
                                const sigil_sync_saves *placed, size_t index);

typedef struct {
    char (*paths)[SIGIL_SAVE_PATH_MAX];
    size_t count;
} sigil_sync_paths;

/* ---- card files (sync_cards.c, sync_folder_cards.c) -------------------------- */

typedef struct {
    char  path[SIGIL_SAVE_PATH_MAX];
    void *card;
    int   format;
    bool  primary;
    bool  changed;
    bool  folder;   /* a PCSX2 folder card: `path` is its directory */
} sigil_sync_card_file;

typedef struct {
    const sigil_sync_kind *kind;
    sigil_sync_card_file   files[SYNC_MAX_CARD_FILES];
    size_t                 count;
    char                   primary_path[SIGIL_SAVE_PATH_MAX];   /* where the game's own card goes */
    char                   problem[SIGIL_SAVE_PATH_MAX];        /* the damaged file, with SIGIL_ERR_DAMAGED */
} sigil_sync_cards;

void sigil_sync_cards_free(sigil_sync_cards *s);
/** The game's card files and the shared ones beside them; `artifact` gets the unit's name. */
int sigil_sync_gather_cards(const sigil_sync_ctx *x, sigil_sync_cards *s, char artifact[SIGIL_SAVE_ENTRY_MAX]);
/** The game's and its companions' saves across the cards, the first card holding a name winning.
 * SIGIL_ERR_DAMAGED, naming the card in s->problem, when one of those saves is corrupt there. */
int sigil_sync_gather_card_saves(const sigil_sync_ctx *x, sigil_sync_cards *s, sigil_sync_saves *out);
int sigil_sync_place_on_cards(const sigil_sync_ctx *x, sigil_sync_cards *cards, sigil_sync_saves *incoming,
                              sigil_sync_result *r);

/** The listing holds files under `dir`/: it is a PCSX2 folder card. */
bool sigil_sync_is_folder_card(const sigil_sync_request *req, const char *dir);
/**
 * Loads folder card `dir` into the next slot of `s` as a card holding the save folders the game sees.
 * SIGIL_ERR_DAMAGED, naming the index in s->problem, for a folder whose _pcsx2_index doesn't parse
 * unless the request says to repair; with repair, that folder is packed without its index.
 */
int sigil_sync_load_folder_card(const sigil_sync_ctx *x, const char *dir, sigil_sync_cards *s);
/**
 * Checks a restore to folder card `f` can run: `removes` gets the files it would remove, and
 * SIGIL_ERR_DAMAGED, naming the superblock in r->problem, when the card holds saves behind an
 * unusable superblock and the request doesn't say to repair.
 */
int sigil_sync_check_folder_card(const sigil_sync_ctx *x, const sigil_sync_card_file *f, size_t *removes,
                                 sigil_sync_result *r);
/** Writes folder card `f`: its superblock when needed, the folders the restore rewrites, and removals. */
int sigil_sync_write_folder_card(const sigil_sync_ctx *x, const sigil_sync_card_file *f);
/**
 * A card holding the save folders of a zip of them, as a client uploaded them before it used sigil's
 * units: members <folder>/<path>, or <card>.ps2/<folder>/<path>. SIGIL_ERR_UNSUPPORTED_FORMAT when a
 * folder doesn't pack.
 */
int sigil_sync_folder_zip_card(const sigil_sync_ctx *x, const sigil_zip_member *members, size_t count, void **card,
                               int *format);

/* ---- volumes (sync_volumes.c) ------------------------------------------------- */

/** Collect on a volume platform: the unit, the holding unit and the owners learned. */
int sigil_sync_collect_volumes(sigil_sync_ctx *x, sigil_sync_result *r);
int sigil_sync_restore_volumes(sigil_sync_ctx *x, sigil_sync_saves *incoming, const size_t sizes[SIGIL_DEVICE_COUNT],
                               sigil_sync_result *r, char local_identity[33]);

/* ---- Dolphin's GCI folder (sync_gci_folder.c) --------------------------------- */

/** The save folder the layout row gives the game; "" when it keeps a card. */
int sigil_sync_save_folder_of(const sigil_sync_ctx *x, char folder[SIGIL_SAVE_PATH_MAX]);
/** The game's and companions' saves in `folder`; a second file with an identity already seen goes in `stale`. */
int sigil_sync_folder_saves(const sigil_sync_ctx *x, const char *folder, sigil_sync_saves *out,
                            sigil_sync_paths *stale);
/** Writes the incoming saves into `folder` and removes the game's files they replace. SIGIL_ERR_NO_SPACE,
 * writing nothing, when Dolphin wouldn't load one of them; r->problem and r->blocks_short say which. */
int sigil_sync_place_in_folder(const sigil_sync_ctx *x, const char *folder, const sigil_sync_saves *local,
                               const sigil_sync_paths *stale, const sigil_sync_saves *incoming, sigil_sync_result *r);

/* ---- folders per user profile (sync_profiles.c) ------------------------------- */

/** Collect on a layout with profiles: the zip of the game's save folders, without the profile's id. */
int sigil_sync_collect_profiles(sigil_sync_ctx *x, const sigil_layout_profiles *row, sigil_sync_result *r);
/**
 * Restore on a layout with profiles: each area the unit carries replaces the game's files in that area,
 * the account area under the chosen profile. `local_identity` gets the identity of the files it found.
 */
int sigil_sync_restore_profiles(sigil_sync_ctx *x, const sigil_layout_profiles *row, const uint8_t *unit, size_t len,
                                sigil_sync_result *r, char local_identity[33]);

/* ---- collect and restore (sync.c) --------------------------------------------- */

/**
 * Saves on disk that changed since the last sync (`last`) stop a restore
 * that would replace them, unless the user chose to overwrite them. `same`
 * is true when they already are what the restore brings.
 */
bool sigil_sync_blocks_restore(const sigil_sync_ctx *x, const char *local, const char *incoming, const char *last,
                               bool *same);

/** The unit and its hashes from the game's saves, and the companions' units. */
int sigil_sync_finish_collect(sigil_sync_ctx *x, const sigil_sync_saves *saves, const sigil_sync_sources *src,
                              sigil_sync_result *r);
/**
 * The conflict rule for the game and for each companion the restore carries a
 * unit for. `local_identity` gets the game's local identity; `already_there`
 * is true when all of them already hold what the restore brings.
 */
int sigil_sync_check_restore(const sigil_sync_ctx *x, const sigil_sync_saves *local, const sigil_sync_saves *incoming,
                             sigil_sync_result *r, char local_identity[33], bool *already_there);

#endif
