// SPDX-License-Identifier: MPL-2.0
/* Ported from CabinetOS's frontend/src/xboxhdd.cpp (MIT; the notice is in
 * licenses/cabinetos-MIT.txt). */
#include "fatx.h"
#include <stdlib.h>

/* A retail drive's E: (xemu_fatx_import.c XEMU_XBOX_HDD_PARTITION_E_OFFSET and
 * _SIZE; libfatx fatx_disk.c fatx_partition_map; fatx.h has the commits). */
#define E_OFFSET        0xABE80000ull
#define E_SIZE          0x1312D6000ull
#define SUPERBLOCK_SIZE 0x1000u
#define ENTRY_SIZE      64u
#define ENTRY_DELETED   0xE5u
#define ENTRY_END       0xFFu
#define ENTRY_UNUSED    0x00u
#define CHAIN_END       0xFFFFFFFFu
#define MAX_DIR_ENTRIES 4096u   /* sigil's own bound on one directory, so a chain that loops ends */
#define MAX_DEPTH       64u

/* The phases a restore's writes go out in (sigil_disk_set_phase). Nothing is
 * pointed at before it is whole and nothing is freed while anything points
 * at it, so a restore in parts that stops after any write leaves every chain
 * whole, each folder it replaces as it was or as it was to be, and clusters
 * marked used that nothing holds as its only waste. libfatx (v0.0.8,
 * fatx_dir.c fatx_unlink) frees a chain and then marks its entry deleted,
 * and its fatx_alloc_cluster takes any free cluster; no source gives a FATX
 * write order that survives a stop, so this one is sigil's. */
#define PHASE_BUILD  0u   /* new files and folders, in clusters free at open, with their FAT entries */
#define PHASE_UNLINK 1u   /* 0xE5 on removed entries no new entry takes the slot of */
#define PHASE_LINK   2u   /* the entries that put the new ones in folders there at open */
#define PHASE_FREE   3u   /* the FAT entries of what was removed, freed */

/* An entry for a folder that was there at open, written in PHASE_LINK. */
typedef struct {
    uint64_t slot;
    uint8_t  raw[ENTRY_SIZE];
} linked_entry;

/* A removed entry: marked 0xE5 in PHASE_UNLINK unless a new entry took its
 * slot, its clusters freed in PHASE_FREE, or at once (sigil_fatx_free_at_once). */
typedef struct {
    sigil_fatx_entry entry;
    bool             replaced;
    bool             freed;
} removed_entry;

struct sigil_fatx {
    sigil_disk    *d;
    uint32_t       cluster_size;
    uint64_t       data_off;     /* where cluster 1 starts on the drive */
    uint32_t       clusters;     /* FAT entries: valid clusters are 1 to clusters - 1 */
    uint32_t       root;
    bool           fat32;
    uint32_t      *fat;          /* every entry, 16-bit markers widened */
    uint8_t       *changed;      /* per entry: 0, or 1 + the phase its new value goes out in */
    uint8_t       *taken;        /* per cluster: taken by this restore, so free at open */
    uint32_t       next;         /* where the search for a free cluster starts */
    uint32_t       free_count;
    uint8_t       *buf;          /* scratch, one cluster */
    linked_entry  *links;
    size_t         link_count, link_cap;
    removed_entry *removed;
    size_t         removed_count, removed_cap;
    bool           flushed;      /* sigil_fatx_flush ran: nothing more is staged */
    bool           at_once;      /* removals free their clusters at once (sigil_fatx_free_at_once) */
};

/* A 16-bit FAT's markers are widened when the FAT is read, so 0xFFFF is CHAIN_END here too. */
static bool is_end(uint32_t v) {
    return v == CHAIN_END;
}

static bool in_volume(const sigil_fatx *fs, uint32_t c) {
    return c >= 1 && c < fs->clusters;
}

static uint64_t offset_of(const sigil_fatx *fs, uint32_t c) {
    return fs->data_off + (uint64_t)(c - 1) * fs->cluster_size;
}

int sigil_fatx_open(sigil_disk *d, sigil_fatx **out) {
    *out = NULL;
    if (sigil_disk_size(d) < E_OFFSET + E_SIZE) return SIGIL_ERR_DAMAGED;
    uint8_t sb[16];
    int rc = sigil_disk_read(d, E_OFFSET, sb, sizeof(sb));
    if (rc != SIGIL_OK) return rc;
    if (memcmp(sb, "FATX", 4) != 0) return SIGIL_ERR_DAMAGED;
    uint32_t sectors = sigil_read_le32(sb + 8);
    if (!sectors || sectors > 1024 || (sectors & (sectors - 1))) return SIGIL_ERR_DAMAGED;
    sigil_fatx *fs = (sigil_fatx *)calloc(1, sizeof(*fs));
    if (!fs) return SIGIL_ERR_OOM;
    fs->d = d;
    fs->cluster_size = sectors * 512u;
    fs->root = sigil_read_le32(sb + 12);
    uint64_t entries = E_SIZE / fs->cluster_size + 1;
    fs->fat32 = entries >= 0xFFF0;
    size_t width = fs->fat32 ? 4 : 2;
    uint64_t fat_bytes = (entries * width + SUPERBLOCK_SIZE - 1) & ~(uint64_t)(SUPERBLOCK_SIZE - 1);
    fs->data_off = E_OFFSET + SUPERBLOCK_SIZE + fat_bytes;
    fs->clusters = (uint32_t)((E_SIZE - fat_bytes - SUPERBLOCK_SIZE) / fs->cluster_size + 1);
    fs->next = 1;
    size_t raw_len = (size_t)fs->clusters * width;
    uint8_t *raw = (uint8_t *)malloc(raw_len);
    fs->fat = (uint32_t *)malloc((size_t)fs->clusters * sizeof(*fs->fat));
    fs->changed = (uint8_t *)calloc(fs->clusters, 1);
    fs->taken = (uint8_t *)calloc(fs->clusters, 1);
    fs->buf = (uint8_t *)malloc(fs->cluster_size);
    rc = raw && fs->fat && fs->changed && fs->taken && fs->buf ? SIGIL_OK : SIGIL_ERR_OOM;
    if (rc == SIGIL_OK) rc = sigil_disk_read(d, E_OFFSET + SUPERBLOCK_SIZE, raw, raw_len);
    for (uint32_t i = 0; rc == SIGIL_OK && i < fs->clusters; i++) {
        uint32_t v = fs->fat32 ? sigil_read_le32(raw + (size_t)i * 4) : sigil_read_le16(raw + (size_t)i * 2);
        if (!fs->fat32 && v >= 0xFFF0) v |= 0xFFFF0000u;
        fs->fat[i] = v;
        if (i >= 1 && v == 0) fs->free_count++;
    }
    free(raw);
    if (rc == SIGIL_OK && !in_volume(fs, fs->root)) rc = SIGIL_ERR_DAMAGED;
    if (rc != SIGIL_OK) {
        sigil_fatx_close(fs);
        return rc;
    }
    *out = fs;
    return SIGIL_OK;
}

void sigil_fatx_close(sigil_fatx *fs) {
    if (!fs) return;
    free(fs->fat);
    free(fs->changed);
    free(fs->taken);
    free(fs->buf);
    free(fs->links);
    free(fs->removed);
    free(fs);
}

uint32_t sigil_fatx_root(const sigil_fatx *fs) {
    return fs->root;
}

uint32_t sigil_fatx_cluster_size(const sigil_fatx *fs) {
    return fs->cluster_size;
}

uint32_t sigil_fatx_free_clusters(const sigil_fatx *fs) {
    return fs->free_count;
}

/* The cluster after `c` in its chain, 0 at the chain's end. SIGIL_ERR_DAMAGED
 * for a chain that leaves the volume or runs into a free, reserved, bad or
 * media entry. */
static int next_of(const sigil_fatx *fs, uint32_t c, uint32_t *next) {
    if (!in_volume(fs, c)) return SIGIL_ERR_DAMAGED;
    uint32_t v = fs->fat[c];
    if (is_end(v)) {
        *next = 0;
        return SIGIL_OK;
    }
    if (!in_volume(fs, v)) return SIGIL_ERR_DAMAGED;
    *next = v;
    return SIGIL_OK;
}

/* The removed entry whose slot is at `at`, or NULL. */
static removed_entry *removed_at(const sigil_fatx *fs, uint64_t at) {
    for (size_t i = 0; i < fs->removed_count; i++) {
        if (fs->removed[i].entry.at == at) return &fs->removed[i];
    }
    return NULL;
}

static void parse_entry(const uint8_t *raw, uint64_t at, sigil_fatx_entry *e) {
    memset(e, 0, sizeof(*e));
    memcpy(e->name, raw + 2, raw[0]);
    e->attr = raw[1];
    e->first = sigil_read_le32(raw + 44);
    e->size = sigil_read_le32(raw + 48);
    e->time = sigil_read_le16(raw + 56);   /* the last write, the second of the three stamps (fatx.h) */
    e->date = sigil_read_le16(raw + 58);
    e->at = at;
}

int sigil_fatx_list(sigil_fatx *fs, uint32_t dir, sigil_fatx_entry **out, size_t *count) {
    *out = NULL;
    *count = 0;
    sigil_fatx_entry *list = NULL;
    size_t n = 0, cap = 0;
    int rc = SIGIL_OK;
    uint32_t seen = 0;
    for (uint32_t c = dir; rc == SIGIL_OK && c;) {
        if (!in_volume(fs, c) || ++seen > MAX_DIR_ENTRIES) {
            rc = SIGIL_ERR_DAMAGED;
            break;
        }
        rc = sigil_disk_read(fs->d, offset_of(fs, c), fs->buf, fs->cluster_size);
        bool ended = false;
        for (size_t i = 0; rc == SIGIL_OK && !ended && i + ENTRY_SIZE <= fs->cluster_size; i += ENTRY_SIZE) {
            uint8_t len = fs->buf[i];
            if (len == ENTRY_END || len == ENTRY_UNUSED) {
                ended = true;
                break;
            }
            if (len == ENTRY_DELETED || len > SIGIL_FATX_NAME_MAX) continue;
            if (removed_at(fs, offset_of(fs, c) + i)) continue;   /* removed, written at flush */
            if (n == cap) {
                cap = cap ? cap * 2 : 16;
                sigil_fatx_entry *grown = (sigil_fatx_entry *)realloc(list, cap * sizeof(*list));
                if (!grown) {
                    rc = SIGIL_ERR_OOM;
                    break;
                }
                list = grown;
            }
            parse_entry(fs->buf + i, offset_of(fs, c) + i, &list[n++]);
        }
        if (rc != SIGIL_OK || ended) break;
        rc = next_of(fs, c, &c);
    }
    if (rc != SIGIL_OK) {
        free(list);
        return rc;
    }
    *out = list;
    *count = n;
    return SIGIL_OK;
}

int sigil_fatx_find(sigil_fatx *fs, uint32_t dir, const char *name, sigil_fatx_entry *out) {
    sigil_fatx_entry *list = NULL;
    size_t n = 0;
    int rc = sigil_fatx_list(fs, dir, &list, &n);
    if (rc != SIGIL_OK) return rc;
    rc = SIGIL_ERR_NOT_FOUND;
    for (size_t i = 0; i < n; i++) {
        if (strcasecmp(list[i].name, name) == 0) {
            *out = list[i];
            rc = SIGIL_OK;
            break;
        }
    }
    free(list);
    return rc;
}

int sigil_fatx_read(sigil_fatx *fs, const sigil_fatx_entry *file, uint8_t **out, size_t *len) {
    *out = NULL;
    *len = 0;
    uint8_t *data = (uint8_t *)malloc(file->size ? file->size : 1);
    if (!data) return SIGIL_ERR_OOM;
    int rc = SIGIL_OK;
    size_t done = 0;
    for (uint32_t c = file->first; rc == SIGIL_OK && done < file->size;) {
        if (!in_volume(fs, c)) {
            rc = SIGIL_ERR_DAMAGED;
            break;
        }
        size_t n = file->size - done < fs->cluster_size ? file->size - done : fs->cluster_size;
        rc = sigil_disk_read(fs->d, offset_of(fs, c), data + done, n);
        done += n;
        if (rc == SIGIL_OK && done < file->size) {
            rc = next_of(fs, c, &c);
            if (rc == SIGIL_OK && !c) rc = SIGIL_ERR_DAMAGED;
        }
    }
    if (rc != SIGIL_OK) {
        free(data);
        return rc;
    }
    *out = data;
    *len = file->size;
    return SIGIL_OK;
}

bool sigil_fatx_name_ok(const char *name) {
    size_t n = strlen(name);
    if (!n || n > SIGIL_FATX_NAME_MAX) return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == 0x7F || strchr("\"*/:<>?\\|", c)) return false;
    }
    return true;
}

/* Sets FAT entry `c`, written to the drive in `phase` (sigil_fatx_flush). */
static void set_fat(sigil_fatx *fs, uint32_t c, uint32_t value, unsigned phase) {
    if (fs->fat[c] == 0 && value != 0) fs->free_count--;
    if (fs->fat[c] != 0 && value == 0) fs->free_count++;
    fs->fat[c] = value;
    fs->changed[c] = (uint8_t)(1 + phase);
}

/* Frees the chain starting at `c`. A chain that is already broken is freed
 * as far as it goes: what it held is going either way. It stops at a cluster
 * this restore took, which a broken chain can run into, since that cluster
 * was free at open. */
static void free_chain(sigil_fatx *fs, uint32_t c) {
    for (uint32_t steps = 0; in_volume(fs, c) && fs->fat[c] != 0 && !fs->taken[c] && steps < fs->clusters; steps++) {
        uint32_t next = fs->fat[c];
        set_fat(fs, c, 0, PHASE_FREE);
        c = next;
    }
}

static int remove_tree(sigil_fatx *fs, const sigil_fatx_entry *e, unsigned depth) {
    if (depth > MAX_DEPTH) return SIGIL_ERR_DAMAGED;
    if (e->attr & SIGIL_FATX_ATTR_DIR) {
        sigil_fatx_entry *kids = NULL;
        size_t n = 0;
        int rc = sigil_fatx_list(fs, e->first, &kids, &n);
        for (size_t i = 0; rc == SIGIL_OK && i < n; i++) rc = remove_tree(fs, &kids[i], depth + 1);
        free(kids);
        if (rc != SIGIL_OK) return rc;
    }
    free_chain(fs, e->first);
    return SIGIL_OK;
}

int sigil_fatx_remove(sigil_fatx *fs, const sigil_fatx_entry *entry) {
    if (fs->flushed) return SIGIL_ERR_INVALID_ARG;
    if (removed_at(fs, entry->at)) return SIGIL_OK;
    if (fs->removed_count == fs->removed_cap) {
        size_t cap = fs->removed_cap ? fs->removed_cap * 2 : 4;
        removed_entry *grown = (removed_entry *)realloc(fs->removed, cap * sizeof(*grown));
        if (!grown) return SIGIL_ERR_OOM;
        fs->removed = grown;
        fs->removed_cap = cap;
    }
    removed_entry *r = &fs->removed[fs->removed_count++];
    r->entry = *entry;
    r->replaced = false;
    r->freed = fs->at_once;
    return fs->at_once ? remove_tree(fs, entry, 0) : SIGIL_OK;
}

void sigil_fatx_free_at_once(sigil_fatx *fs) {
    fs->at_once = true;
}

/* A free cluster, marked as the end of a chain; 0 when E: is full. It was
 * free at open: what a restore removes is freed by sigil_fatx_flush, after
 * the last cluster is taken, unless sigil_fatx_free_at_once frees it at once. */
static uint32_t take_cluster(sigil_fatx *fs) {
    for (uint32_t i = 0; i + 1 < fs->clusters; i++) {
        uint32_t c = 1 + (fs->next - 1 + i) % (fs->clusters - 1);
        if (fs->fat[c] == 0) {
            set_fat(fs, c, CHAIN_END, PHASE_BUILD);
            fs->taken[c] = 1;
            fs->next = c;
            return c;
        }
    }
    return 0;
}

/* Sets the first byte of the slot after `slot` to the end marker when that
 * slot isn't one already, so entries a longer directory once held past its
 * end stay out of it. */
static int end_after(sigil_fatx *fs, uint32_t cluster, uint64_t slot) {
    uint64_t next = slot + ENTRY_SIZE;
    if (next >= offset_of(fs, cluster) + fs->cluster_size) {
        uint32_t following = 0;
        int rc = next_of(fs, cluster, &following);
        if (rc != SIGIL_OK || !following) return rc;
        next = offset_of(fs, following);
    }
    uint8_t first = 0;
    int rc = sigil_disk_read(fs->d, next, &first, 1);
    if (rc != SIGIL_OK || first == ENTRY_END || first == ENTRY_UNUSED) return rc;
    first = ENTRY_END;
    return sigil_disk_write(fs->d, next, &first, 1);
}

static bool slot_linked(const sigil_fatx *fs, uint64_t slot) {
    for (size_t i = 0; i < fs->link_count; i++) {
        if (fs->links[i].slot == slot) return true;
    }
    return false;
}

/* A slot for a new entry in directory `dir`: its first deleted or end slot
 * that no entry waiting for PHASE_LINK holds, else the start of a cluster
 * added to it. sigil doesn't grow the root (one cluster on xemu-blank and
 * every sample drive) or a directory past MAX_DIR_ENTRIES: both are refused
 * as no space. A cluster added to a directory that was there at open joins
 * its chain in PHASE_LINK, with the entries it holds. */
static int free_slot(sigil_fatx *fs, uint32_t dir, uint64_t *slot) {
    uint32_t c = dir, last = dir, seen = 0;
    while (c) {
        if (!in_volume(fs, c) || ++seen > MAX_DIR_ENTRIES) return SIGIL_ERR_DAMAGED;
        int rc = sigil_disk_read(fs->d, offset_of(fs, c), fs->buf, fs->cluster_size);
        if (rc != SIGIL_OK) return rc;
        for (size_t i = 0; i + ENTRY_SIZE <= fs->cluster_size; i += ENTRY_SIZE) {
            uint8_t len = fs->buf[i];
            if (len != ENTRY_DELETED && len != ENTRY_END && len != ENTRY_UNUSED) continue;
            if (slot_linked(fs, offset_of(fs, c) + i)) continue;
            *slot = offset_of(fs, c) + i;
            return len == ENTRY_DELETED ? SIGIL_OK : end_after(fs, c, *slot);
        }
        last = c;
        rc = next_of(fs, c, &c);
        if (rc != SIGIL_OK) return rc;
    }
    if (dir == fs->root || (uint64_t)seen * (fs->cluster_size / ENTRY_SIZE) >= MAX_DIR_ENTRIES) return SIGIL_ERR_NO_SPACE;
    uint32_t grown = take_cluster(fs);
    if (!grown) return SIGIL_ERR_NO_SPACE;
    set_fat(fs, last, grown, fs->taken[dir] ? PHASE_BUILD : PHASE_LINK);
    memset(fs->buf, ENTRY_END, fs->cluster_size);
    *slot = offset_of(fs, grown);
    return sigil_disk_write(fs->d, *slot, fs->buf, fs->cluster_size);
}

/* Slot `slot` lies in the chain of directory `dir`. */
static bool slot_in_dir(const sigil_fatx *fs, uint32_t dir, uint64_t slot) {
    uint32_t seen = 0;
    for (uint32_t c = dir; in_volume(fs, c) && seen++ < MAX_DIR_ENTRIES;) {
        if (slot >= offset_of(fs, c) && slot < offset_of(fs, c) + fs->cluster_size) return true;
        if (next_of(fs, c, &c) != SIGIL_OK) break;
    }
    return false;
}

/* Writes the entry into a directory this restore made, which nothing points
 * at yet; into a directory there at open it waits for PHASE_LINK, in the
 * slot of a removed entry of the same name, so the two never show together,
 * or in a free one. */
static int add_entry(sigil_fatx *fs, uint32_t dir, const char *name, uint8_t attr, uint32_t first, uint32_t size,
                     uint16_t time, uint16_t date) {
    uint8_t raw[ENTRY_SIZE];
    size_t n = strlen(name);
    memset(raw, 0xFF, sizeof(raw));
    raw[0] = (uint8_t)n;
    raw[1] = attr;
    memcpy(raw + 2, name, n);
    sigil_write_le32(raw + 44, first);
    sigil_write_le32(raw + 48, size);
    for (size_t i = 0; i < 3; i++) {   /* creation, last write, last access: equal, as on a kernel-made entry */
        sigil_write_le16(raw + 52 + i * 4, time);
        sigil_write_le16(raw + 54 + i * 4, date);
    }
    uint64_t slot = 0;
    int rc = SIGIL_OK;
    if (fs->taken[dir]) {
        rc = free_slot(fs, dir, &slot);
        return rc == SIGIL_OK ? sigil_disk_write(fs->d, slot, raw, sizeof(raw)) : rc;
    }
    removed_entry *same = NULL;
    for (size_t i = 0; i < fs->removed_count && !same; i++) {
        removed_entry *r = &fs->removed[i];
        if (!r->replaced && strcasecmp(r->entry.name, name) == 0 && slot_in_dir(fs, dir, r->entry.at)) same = r;
    }
    if (same) {
        same->replaced = true;
        slot = same->entry.at;
    } else {
        rc = free_slot(fs, dir, &slot);
    }
    if (rc != SIGIL_OK) return rc;
    if (fs->link_count == fs->link_cap) {
        size_t cap = fs->link_cap ? fs->link_cap * 2 : 4;
        linked_entry *grown = (linked_entry *)realloc(fs->links, cap * sizeof(*grown));
        if (!grown) return SIGIL_ERR_OOM;
        fs->links = grown;
        fs->link_cap = cap;
    }
    fs->links[fs->link_count].slot = slot;
    memcpy(fs->links[fs->link_count].raw, raw, sizeof(raw));
    fs->link_count++;
    return SIGIL_OK;
}

int sigil_fatx_mkdir(sigil_fatx *fs, uint32_t dir, const char *name, uint16_t time, uint16_t date, uint32_t *made) {
    if (fs->flushed) return SIGIL_ERR_INVALID_ARG;
    if (!sigil_fatx_name_ok(name)) return SIGIL_ERR_INVALID_ARG;
    uint32_t c = take_cluster(fs);
    if (!c) return SIGIL_ERR_NO_SPACE;
    memset(fs->buf, ENTRY_END, fs->cluster_size);   /* which reads as an empty directory */
    int rc = sigil_disk_write(fs->d, offset_of(fs, c), fs->buf, fs->cluster_size);
    if (rc == SIGIL_OK) rc = add_entry(fs, dir, name, SIGIL_FATX_ATTR_DIR, c, 0, time, date);
    if (rc == SIGIL_OK) *made = c;
    return rc;
}

int sigil_fatx_put(sigil_fatx *fs, uint32_t dir, const char *name, const uint8_t *data, size_t len, uint16_t time,
                   uint16_t date) {
    if (fs->flushed) return SIGIL_ERR_INVALID_ARG;
    if (!sigil_fatx_name_ok(name) || len > 0xFFFFFFFFu) return SIGIL_ERR_INVALID_ARG;
    uint32_t first = 0, last = 0;
    int rc = SIGIL_OK;
    for (size_t done = 0; rc == SIGIL_OK && done < len;) {
        uint32_t c = take_cluster(fs);
        if (!c) return SIGIL_ERR_NO_SPACE;
        if (last) set_fat(fs, last, c, PHASE_BUILD);
        else first = c;
        last = c;
        size_t n = len - done < fs->cluster_size ? len - done : fs->cluster_size;
        memcpy(fs->buf, data + done, n);
        memset(fs->buf + n, 0, fs->cluster_size - n);
        rc = sigil_disk_write(fs->d, offset_of(fs, c), fs->buf, fs->cluster_size);
        done += n;
    }
    if (rc == SIGIL_OK) rc = add_entry(fs, dir, name, 0, first, (uint32_t)len, time, date);
    return rc;
}

/* Stages the FAT entries whose new value goes out in `phase`. */
static int write_fat(sigil_fatx *fs, unsigned phase) {
    size_t width = fs->fat32 ? 4 : 2;
    uint8_t raw[1024];
    int rc = SIGIL_OK;
    for (uint32_t i = 0; rc == SIGIL_OK && i < fs->clusters;) {
        if (fs->changed[i] != 1 + phase) {
            i++;
            continue;
        }
        uint32_t start = i;
        size_t n = 0;
        for (; i < fs->clusters && fs->changed[i] == 1 + phase && n + width <= sizeof(raw); i++, n += width) {
            if (fs->fat32) sigil_write_le32(raw + n, fs->fat[i]);
            else sigil_write_le16(raw + n, (uint16_t)fs->fat[i]);
        }
        rc = sigil_disk_write(fs->d, E_OFFSET + SUPERBLOCK_SIZE + (uint64_t)start * width, raw, n);
    }
    return rc;
}

int sigil_fatx_flush(sigil_fatx *fs) {
    if (fs->flushed) return SIGIL_ERR_INVALID_ARG;
    fs->flushed = true;
    int rc = write_fat(fs, PHASE_BUILD);
    /* The other spellings of a replaced folder go before the new entry shows. */
    if (rc == SIGIL_OK) rc = sigil_disk_set_phase(fs->d, PHASE_UNLINK);
    const uint8_t mark = ENTRY_DELETED;
    for (size_t i = 0; rc == SIGIL_OK && i < fs->removed_count; i++) {
        if (!fs->removed[i].replaced) rc = sigil_disk_write(fs->d, fs->removed[i].entry.at, &mark, 1);
    }
    if (rc == SIGIL_OK) rc = sigil_disk_set_phase(fs->d, PHASE_LINK);
    for (size_t i = 0; rc == SIGIL_OK && i < fs->link_count; i++) {
        rc = sigil_disk_write(fs->d, fs->links[i].slot, fs->links[i].raw, ENTRY_SIZE);
    }
    if (rc == SIGIL_OK) rc = write_fat(fs, PHASE_LINK);
    if (rc == SIGIL_OK) rc = sigil_disk_set_phase(fs->d, PHASE_FREE);
    /* The removed slots are written now, some with new entries: no listing hides them any more. */
    size_t removed = fs->removed_count;
    fs->removed_count = 0;
    for (size_t i = 0; rc == SIGIL_OK && i < removed; i++) {
        if (!fs->removed[i].freed) rc = remove_tree(fs, &fs->removed[i].entry, 0);
    }
    if (rc == SIGIL_OK) rc = write_fat(fs, PHASE_FREE);
    fs->link_count = 0;
    memset(fs->changed, 0, fs->clusters);
    return rc;
}
