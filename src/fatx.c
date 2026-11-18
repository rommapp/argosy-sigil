// SPDX-License-Identifier: MPL-2.0
/* Ported from CabinetOS's frontend/src/xboxhdd.cpp (MIT; the notice is in
 * licenses/cabinetos-MIT.txt). */
#include "fatx.h"
#include <stdlib.h>

/* A retail drive's E: (xemu's own image, and every drive the Xbox formats). */
#define E_OFFSET        0xABE80000ull
#define E_SIZE          0x131F00000ull
#define SUPERBLOCK_SIZE 0x1000u
#define ENTRY_SIZE      64u
#define ENTRY_DELETED   0xE5u
#define ENTRY_END       0xFFu
#define ENTRY_UNUSED    0x00u
#define CHAIN_END       0xFFFFFFFFu
#define MAX_DIR_ENTRIES 4096u   /* the kernel's limit for one directory */
#define MAX_DEPTH       64u

struct sigil_fatx {
    sigil_disk *d;
    uint32_t    cluster_size;
    uint64_t    data_off;     /* where cluster 1 starts on the drive */
    uint32_t    clusters;     /* FAT entries: valid clusters are 1 to clusters - 1 */
    uint32_t    root;
    bool        fat32;
    uint32_t   *fat;          /* every entry, 16-bit markers widened */
    uint8_t    *changed;      /* one flag per entry */
    uint32_t    next;         /* where the search for a free cluster starts */
    uint32_t    free_count;
    uint8_t    *buf;          /* scratch, one cluster */
};

static bool is_end(uint32_t v) {
    return v >= 0xFFFFFFF0u;
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
    fs->buf = (uint8_t *)malloc(fs->cluster_size);
    rc = raw && fs->fat && fs->changed && fs->buf ? SIGIL_OK : SIGIL_ERR_OOM;
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
    free(fs->buf);
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
 * for a chain that leaves the volume or runs into a free cluster. */
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

static void parse_entry(const uint8_t *raw, uint64_t at, sigil_fatx_entry *e) {
    memset(e, 0, sizeof(*e));
    memcpy(e->name, raw + 2, raw[0]);
    e->attr = raw[1];
    e->first = sigil_read_le32(raw + 44);
    e->size = sigil_read_le32(raw + 48);
    e->time = sigil_read_le16(raw + 56);
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

static void set_fat(sigil_fatx *fs, uint32_t c, uint32_t value) {
    if (fs->fat[c] == 0 && value != 0) fs->free_count--;
    if (fs->fat[c] != 0 && value == 0) fs->free_count++;
    fs->fat[c] = value;
    fs->changed[c] = 1;
}

/* Frees the chain starting at `c`. A chain that is already broken is freed
 * as far as it goes: what it held is going either way. */
static void free_chain(sigil_fatx *fs, uint32_t c) {
    for (uint32_t steps = 0; in_volume(fs, c) && fs->fat[c] != 0 && steps < fs->clusters; steps++) {
        uint32_t next = fs->fat[c];
        set_fat(fs, c, 0);
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
    int rc = remove_tree(fs, entry, 0);
    if (rc != SIGIL_OK) return rc;
    const uint8_t mark = ENTRY_DELETED;
    return sigil_disk_write(fs->d, entry->at, &mark, 1);
}

/* A free cluster, marked as the end of a chain; 0 when E: is full. */
static uint32_t take_cluster(sigil_fatx *fs) {
    for (uint32_t i = 0; i + 1 < fs->clusters; i++) {
        uint32_t c = 1 + (fs->next - 1 + i) % (fs->clusters - 1);
        if (fs->fat[c] == 0) {
            set_fat(fs, c, CHAIN_END);
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

/* A slot for a new entry in directory `dir`: its first deleted or end slot,
 * else the start of a cluster added to it. The root can't grow. */
static int free_slot(sigil_fatx *fs, uint32_t dir, uint64_t *slot) {
    uint32_t c = dir, last = dir, seen = 0;
    while (c) {
        if (!in_volume(fs, c) || ++seen > MAX_DIR_ENTRIES) return SIGIL_ERR_DAMAGED;
        int rc = sigil_disk_read(fs->d, offset_of(fs, c), fs->buf, fs->cluster_size);
        if (rc != SIGIL_OK) return rc;
        for (size_t i = 0; i + ENTRY_SIZE <= fs->cluster_size; i += ENTRY_SIZE) {
            uint8_t len = fs->buf[i];
            if (len != ENTRY_DELETED && len != ENTRY_END && len != ENTRY_UNUSED) continue;
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
    set_fat(fs, last, grown);
    memset(fs->buf, ENTRY_END, fs->cluster_size);
    *slot = offset_of(fs, grown);
    return sigil_disk_write(fs->d, *slot, fs->buf, fs->cluster_size);
}

static int add_entry(sigil_fatx *fs, uint32_t dir, const char *name, uint8_t attr, uint32_t first, uint32_t size,
                     uint16_t time, uint16_t date) {
    uint64_t slot = 0;
    int rc = free_slot(fs, dir, &slot);
    if (rc != SIGIL_OK) return rc;
    uint8_t raw[ENTRY_SIZE];
    size_t n = strlen(name);
    memset(raw, 0xFF, sizeof(raw));
    raw[0] = (uint8_t)n;
    raw[1] = attr;
    memcpy(raw + 2, name, n);
    sigil_write_le32(raw + 44, first);
    sigil_write_le32(raw + 48, size);
    for (size_t i = 0; i < 3; i++) {   /* created, written, read */
        sigil_write_le16(raw + 52 + i * 4, time);
        sigil_write_le16(raw + 54 + i * 4, date);
    }
    return sigil_disk_write(fs->d, slot, raw, sizeof(raw));
}

int sigil_fatx_mkdir(sigil_fatx *fs, uint32_t dir, const char *name, uint16_t time, uint16_t date, uint32_t *made) {
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
    if (!sigil_fatx_name_ok(name) || len > 0xFFFFFFFFu) return SIGIL_ERR_INVALID_ARG;
    uint32_t first = 0, last = 0;
    int rc = SIGIL_OK;
    for (size_t done = 0; rc == SIGIL_OK && done < len;) {
        uint32_t c = take_cluster(fs);
        if (!c) return SIGIL_ERR_NO_SPACE;
        if (last) set_fat(fs, last, c);
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

int sigil_fatx_flush(sigil_fatx *fs) {
    size_t width = fs->fat32 ? 4 : 2;
    uint8_t raw[1024];
    int rc = SIGIL_OK;
    for (uint32_t i = 0; rc == SIGIL_OK && i < fs->clusters;) {
        if (!fs->changed[i]) {
            i++;
            continue;
        }
        uint32_t start = i;
        size_t n = 0;
        for (; i < fs->clusters && fs->changed[i] && n + width <= sizeof(raw); i++, n += width) {
            if (fs->fat32) sigil_write_le32(raw + n, fs->fat[i]);
            else sigil_write_le16(raw + n, (uint16_t)fs->fat[i]);
            fs->changed[i] = 0;
        }
        rc = sigil_disk_write(fs->d, E_OFFSET + SUPERBLOCK_SIZE + (uint64_t)start * width, raw, n);
    }
    return rc;
}
