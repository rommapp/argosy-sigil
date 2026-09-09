// SPDX-License-Identifier: MPL-2.0
#include "sigil_internal.h"
#include "sigil_compat.h"
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

#define SIGIL_VERSION_STRING "0.1.0-dev"
#define SIGIL_DIR_SCAN_MAX_DEPTH 4
/* Matched as a path suffix so the variable app/<TITLEID>/ prefix in a dump
 * does not have to be known ahead of time. */
#define VITA_SFO_MEMBER "sce_sys/param.sfo"

typedef struct {
    sigil_platform p;
    const char    *slug;
} platform_slug;

static const platform_slug PLATFORM_SLUGS[] = {
    { SIGIL_PLATFORM_AUTO,     "auto"     },
    { SIGIL_PLATFORM_PSP,      "psp"      },
    { SIGIL_PLATFORM_PSX,      "psx"      },
    { SIGIL_PLATFORM_PS2,      "ps2"      },
    { SIGIL_PLATFORM_PSVITA,   "psvita"   },
    { SIGIL_PLATFORM_SWITCH,   "switch"   },
    { SIGIL_PLATFORM_3DS,      "3ds"      },
    { SIGIL_PLATFORM_WII,      "wii"      },
    { SIGIL_PLATFORM_WIIU,     "wiiu"     },
    { SIGIL_PLATFORM_GAMECUBE, "gamecube" },
    { SIGIL_PLATFORM_PS3,      "ps3"      },
    { SIGIL_PLATFORM_XBOX360,  "xbox360"  },
    { SIGIL_PLATFORM_DREAMCAST, "dreamcast" },
    { SIGIL_PLATFORM_XBOX,     "xbox"     },
    { SIGIL_PLATFORM_GB,       "gb"       },
    { SIGIL_PLATFORM_GBC,      "gbc"      },
    { SIGIL_PLATFORM_SNES,     "snes"     },
};
static const size_t PLATFORM_SLUG_COUNT = sizeof(PLATFORM_SLUGS) / sizeof(PLATFORM_SLUGS[0]);

/* Aliases consumers may use (argosy uses "vita"/"ngc"/"dc" internally). */
static const platform_slug PLATFORM_ALIASES[] = {
    { SIGIL_PLATFORM_PSVITA,   "vita" },
    { SIGIL_PLATFORM_GAMECUBE, "ngc"  },
    { SIGIL_PLATFORM_GAMECUBE, "gc"   },
    { SIGIL_PLATFORM_3DS,      "n3ds" },
    { SIGIL_PLATFORM_SWITCH,   "nsw"  },
    { SIGIL_PLATFORM_XBOX360,  "x360" },
    { SIGIL_PLATFORM_DREAMCAST, "dc"   },
    { SIGIL_PLATFORM_XBOX,      "xbx"  },
    { SIGIL_PLATFORM_SNES,      "sfc"  },
    { SIGIL_PLATFORM_SNES,      "sfam" },
};
static const size_t PLATFORM_ALIAS_COUNT = sizeof(PLATFORM_ALIASES) / sizeof(PLATFORM_ALIASES[0]);

sigil_platform sigil_platform_from_slug(const char *slug) {
    if (!slug) return SIGIL_PLATFORM_AUTO;
    for (size_t i = 0; i < PLATFORM_SLUG_COUNT; i++) {
        if (strcmp(slug, PLATFORM_SLUGS[i].slug) == 0) return PLATFORM_SLUGS[i].p;
    }
    for (size_t i = 0; i < PLATFORM_ALIAS_COUNT; i++) {
        if (strcmp(slug, PLATFORM_ALIASES[i].slug) == 0) return PLATFORM_ALIASES[i].p;
    }
    return SIGIL_PLATFORM_AUTO;
}

const char *sigil_platform_to_slug(sigil_platform p) {
    for (size_t i = 0; i < PLATFORM_SLUG_COUNT; i++) {
        if (PLATFORM_SLUGS[i].p == p) return PLATFORM_SLUGS[i].slug;
    }
    return "auto";
}

const char *sigil_version(void) { return SIGIL_VERSION_STRING; }

const char *sigil_strerror(int code) {
    switch (code) {
    case SIGIL_OK:                       return "ok";
    case SIGIL_ERR_INVALID_ARG:          return "invalid argument";
    case SIGIL_ERR_IO:                   return "I/O error";
    case SIGIL_ERR_UNKNOWN_PLATFORM:     return "unknown platform";
    case SIGIL_ERR_UNSUPPORTED_FORMAT:   return "unsupported format";
    case SIGIL_ERR_NOT_FOUND:            return "title id not found";
    case SIGIL_ERR_NEEDS_KEY:            return "decryption key required";
    case SIGIL_ERR_CRYPTO:               return "crypto failure";
    case SIGIL_ERR_OOM:                  return "out of memory";
    default:                             return "unknown error";
    }
}

static const char *path_basename(const char *path) {
    if (!path) return NULL;
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *bslash = strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    return slash ? slash + 1 : path;
}

static bool name_has_suffix(const char *name, const char *suffix) {
    if (!name) return false;
    size_t n = strlen(name), s = strlen(suffix);
    if (n < s) return false;
    const char *tail = name + (n - s);
    for (size_t i = 0; i < s; i++) {
        if (sigil_to_lower(tail[i]) != sigil_to_lower(suffix[i])) return false;
    }
    return true;
}

/* `.7z` is listed as the hook point for a reader that does not exist yet, so
 * today it simply fails to open and falls through to the filename scanner.
 * Adding one means vendoring the LZMA SDK's container sources (7zArcIn.c,
 * 7zDec.c, Lzma2Dec.c and friends); third_party currently carries only the
 * LzmaDec.c that libchdr needs. */
static bool path_is_archive(const char *filename) {
    char ext[16];
    sigil_lower_ext(filename, ext);
    return strcmp(ext, "zip") == 0 || strcmp(ext, "7z") == 0;
}

static sigil_platform sniff_from_extension(const char *filename) {
    char ext[16];
    sigil_lower_ext(filename, ext);
    if (ext[0] == '\0') return SIGIL_PLATFORM_AUTO;

    if (strcmp(ext, "nsp") == 0)   return SIGIL_PLATFORM_SWITCH;
    if (strcmp(ext, "xci") == 0)   return SIGIL_PLATFORM_SWITCH;
    if (strcmp(ext, "wua") == 0)   return SIGIL_PLATFORM_WIIU;
    if (strcmp(ext, "3ds") == 0)   return SIGIL_PLATFORM_3DS;
    if (strcmp(ext, "cci") == 0)   return SIGIL_PLATFORM_3DS;
    if (strcmp(ext, "z3ds") == 0)  return SIGIL_PLATFORM_3DS;
    if (strcmp(ext, "zcci") == 0)  return SIGIL_PLATFORM_3DS;
    if (strcmp(ext, "cxi") == 0)   return SIGIL_PLATFORM_3DS;
    if (strcmp(ext, "zcxi") == 0)  return SIGIL_PLATFORM_3DS;
    if (strcmp(ext, "app") == 0)   return SIGIL_PLATFORM_3DS;
    if (strcmp(ext, "3dsx") == 0)  return SIGIL_PLATFORM_3DS;
    if (strcmp(ext, "z3dsx") == 0) return SIGIL_PLATFORM_3DS;
    if (strcmp(ext, "rvz") == 0)   return SIGIL_PLATFORM_WII;
    if (strcmp(ext, "wbfs") == 0)  return SIGIL_PLATFORM_WII;
    if (strcmp(ext, "wad") == 0)   return SIGIL_PLATFORM_WII;
    if (strcmp(ext, "cso") == 0)   return SIGIL_PLATFORM_PSP;
    if (strcmp(ext, "ciso") == 0)  return SIGIL_PLATFORM_PSP;
    if (strcmp(ext, "sfo") == 0)   return SIGIL_PLATFORM_PS3;
    if (strcmp(ext, "xex") == 0)   return SIGIL_PLATFORM_XBOX360;
    if (strcmp(ext, "xbe") == 0)   return SIGIL_PLATFORM_XBOX;
    if (strcmp(ext, "xiso") == 0)  return SIGIL_PLATFORM_XBOX;
    /* ZArchive under two names: Cemu writes .wua for Wii U, Xenia writes .zar
     * for the 360, so the extension is what separates them. */
    if (strcmp(ext, "zar") == 0)   return SIGIL_PLATFORM_XBOX360;
    if (strcmp(ext, "gdi") == 0)   return SIGIL_PLATFORM_DREAMCAST;
    if (strcmp(ext, "cdi") == 0)   return SIGIL_PLATFORM_DREAMCAST;
    if (strcmp(ext, "gb") == 0)    return SIGIL_PLATFORM_GB;
    if (strcmp(ext, "sgb") == 0)   return SIGIL_PLATFORM_GB;
    if (strcmp(ext, "gbc") == 0)   return SIGIL_PLATFORM_GBC;
    if (strcmp(ext, "sfc") == 0)   return SIGIL_PLATFORM_SNES;
    if (strcmp(ext, "smc") == 0)   return SIGIL_PLATFORM_SNES;

    /* `Game.xiso.iso` is a real convention in Xbox sets, and the trailing
     * `.iso` alone would throw away what the name already states. */
    if (name_has_suffix(filename, ".xiso.iso")) return SIGIL_PLATFORM_XBOX;

    /* `.iso`/`.bin`/`.chd` are ambiguous between PSP/PSX/PS2/Wii/GC/Dreamcast/Xbox;
     * refuse to guess without a hint. `.elf`/`.axf` are 3DS homebrew to azahar
     * but generic everywhere else, so they need an explicit platform hint. */
    return SIGIL_PLATFORM_AUTO;
}

static sigil_io *open_io_for_platform(const char *path, sigil_platform p) {
    if (!path) return NULL;
    char ext[16];
    sigil_lower_ext(path_basename(path), ext);

    bool can_chd = (p == SIGIL_PLATFORM_PSP || p == SIGIL_PLATFORM_PSX
                    || p == SIGIL_PLATFORM_PS2 || p == SIGIL_PLATFORM_DREAMCAST);

    if (can_chd && strcmp(ext, "chd") == 0) {
        sigil_io *io = sigil_io_open_chd(path);
        if (io) return io;
    }
    if (p == SIGIL_PLATFORM_PSP && (strcmp(ext, "cso") == 0 || strcmp(ext, "ciso") == 0)) {
        sigil_io *io = sigil_io_open_cso(path);
        if (io) return io;
    }
    /* A .zar holds the extracted disc filesystem rather than a disc image, so
     * the boot executable is a member and the walker never runs. */
    if (p == SIGIL_PLATFORM_XBOX360 && strcmp(ext, "zar") == 0) {
        sigil_io *io = sigil_io_open_zar(path, "default.xex");
        if (io) return io;
    }
    /* A Dreamcast data track dumped alongside a .gdi is raw 2352-byte MODE1,
     * like a PSX .bin; the raw-CD layer cooks it and passes a plain 2048-byte
     * image through untouched. */
    if ((p == SIGIL_PLATFORM_PSX || p == SIGIL_PLATFORM_DREAMCAST)
        && strcmp(ext, "bin") == 0) {
        sigil_io *io = sigil_io_open_raw_cd(path);
        if (io) return io;
    }
    return sigil_io_open_file(path);
}

static bool path_is_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

#ifdef _WIN32
static int find_file_in_dir(const char *dir, const char *target_name,
                            char *out, size_t out_cap, int depth) {
    if (depth > SIGIL_DIR_SCAN_MAX_DEPTH) return SIGIL_ERR_NOT_FOUND;
    char pattern[1024];
    int pn = snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    if (pn <= 0 || (size_t)pn >= sizeof(pattern)) return SIGIL_ERR_IO;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return SIGIL_ERR_IO;
    int found = SIGIL_ERR_NOT_FOUND;
    do {
        const char *name = fd.cFileName;
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) continue;
        char child[1024];
        int n = snprintf(child, sizeof(child), "%s\\%s", dir, name);
        if (n <= 0 || (size_t)n >= sizeof(child)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (find_file_in_dir(child, target_name, out, out_cap, depth + 1) == SIGIL_OK) {
                found = SIGIL_OK;
                break;
            }
        } else if (strcasecmp(name, target_name) == 0) {
            size_t need = (size_t)n + 1;
            if (need > out_cap) continue;
            memcpy(out, child, need);
            found = SIGIL_OK;
            break;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}
#else
static int find_file_in_dir(const char *dir, const char *target_name,
                            char *out, size_t out_cap, int depth) {
    if (depth > SIGIL_DIR_SCAN_MAX_DEPTH) return SIGIL_ERR_NOT_FOUND;
    DIR *dp = opendir(dir);
    if (!dp) return SIGIL_ERR_IO;
    struct dirent *e;
    int found = SIGIL_ERR_NOT_FOUND;
    while ((e = readdir(dp)) != NULL) {
        const char *name = e->d_name;
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) continue;
        char child[1024];
        int n = snprintf(child, sizeof(child), "%s/%s", dir, name);
        if (n <= 0 || (size_t)n >= sizeof(child)) continue;
        struct stat st;
        if (stat(child, &st) != 0) continue;
        if (S_ISREG(st.st_mode) && strcasecmp(name, target_name) == 0) {
            size_t need = (size_t)n + 1;
            if (need > out_cap) continue;
            memcpy(out, child, need);
            found = SIGIL_OK;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            if (find_file_in_dir(child, target_name, out, out_cap, depth + 1) == SIGIL_OK) {
                found = SIGIL_OK;
                break;
            }
        }
    }
    closedir(dp);
    return found;
}
#endif

static const char *directory_target_for_platform(sigil_platform p) {
    switch (p) {
    case SIGIL_PLATFORM_PS3:     return "PARAM.SFO";
    case SIGIL_PLATFORM_XBOX360: return "default.xex";
    case SIGIL_PLATFORM_XBOX:    return "default.xbe";
    default:                      return NULL;
    }
}

static int dispatch(const sigil_io *io, const char *filename_hint,
                    sigil_platform p, const sigil_options *opts,
                    sigil_result *out) {
    switch (p) {
    case SIGIL_PLATFORM_PSP:      return sigil_extract_psp(io, filename_hint, opts, out);
    case SIGIL_PLATFORM_PSX:      return sigil_extract_psx(io, filename_hint, opts, out);
    case SIGIL_PLATFORM_PS2:      return sigil_extract_ps2(io, filename_hint, opts, out);
    case SIGIL_PLATFORM_WII:      return sigil_extract_wii(io, filename_hint, opts, out);
    case SIGIL_PLATFORM_GAMECUBE: return sigil_extract_gamecube(io, filename_hint, opts, out);
    case SIGIL_PLATFORM_3DS:      return sigil_extract_3ds(io, filename_hint, opts, out);
    case SIGIL_PLATFORM_SWITCH:   return sigil_extract_switch(io, filename_hint, opts, out);
    case SIGIL_PLATFORM_WIIU:     return sigil_extract_wiiu(io, filename_hint, opts, out);
    case SIGIL_PLATFORM_PSVITA:   return sigil_extract_psvita(io, filename_hint, opts, out);
    case SIGIL_PLATFORM_PS3:      return sigil_extract_ps3(io, filename_hint, opts, out);
    case SIGIL_PLATFORM_XBOX360:  return sigil_extract_xbox360(io, filename_hint, opts, out);
    case SIGIL_PLATFORM_DREAMCAST: return sigil_extract_dreamcast(io, filename_hint, opts, out);
    case SIGIL_PLATFORM_XBOX:     return sigil_extract_xbox(io, filename_hint, opts, out);
    case SIGIL_PLATFORM_GB:       return sigil_extract_gb(io, filename_hint, opts, out);
    case SIGIL_PLATFORM_GBC: {
        int rc = sigil_extract_gb(io, filename_hint, opts, out);
        if (rc == SIGIL_OK) out->platform = SIGIL_PLATFORM_GBC;
        return rc;
    }
    case SIGIL_PLATFORM_SNES:     return sigil_extract_snes(io, filename_hint, opts, out);
    default:                       return SIGIL_ERR_UNKNOWN_PLATFORM;
    }
}

int sigil_extract_from_io(const sigil_io *io,
                          const char *filename_hint,
                          sigil_platform hint,
                          const sigil_options *opts,
                          sigil_result *out) {
    if (!io || !io->read || !out) return SIGIL_ERR_INVALID_ARG;

    if (hint == SIGIL_PLATFORM_AUTO) {
        hint = sniff_from_extension(filename_hint);
    }
    if (hint == SIGIL_PLATFORM_AUTO) return SIGIL_ERR_UNKNOWN_PLATFORM;

    int rc = dispatch(io, filename_hint, hint, opts, out);

    /* Filename fallback runs on any non-OK result when the flag is set
     * (default for opts == NULL). */
    bool fallback_enabled = !opts || (opts->flags & SIGIL_FLAG_FILENAME_FALLBACK);
    if (rc != SIGIL_OK && fallback_enabled && filename_hint) {
        sigil_result fb;
        if (sigil_filename_fallback(filename_hint, hint, &fb) == SIGIL_OK) {
            *out = fb;
            rc = SIGIL_OK;
        }
    }

    /* Default save_id to title_id when no platform-specific value was set.
     * A folder-split platform is excluded: its save location is a nested path
     * and a flat title id is not one, so backfilling would turn "no save
     * location was resolved" into a wrong directory name. There save_id stays
     * empty, which the header documents as unknown. */
    if (rc == SIGIL_OK && out->save_id[0] == '\0' && out->usage != SIGIL_USAGE_FOLDER_SPLIT) {
        size_t len = strlen(out->title_id);
        if (len < sizeof(out->save_id)) {
            memcpy(out->save_id, out->title_id, len + 1);
        }
    }

    /* Flag CSO-sourced PSP extractions as experimental until verified against real ROMs. */
    if (rc == SIGIL_OK && filename_hint) {
        char ext[16];
        sigil_lower_ext(filename_hint, ext);
        if (strcmp(ext, "cso") == 0 || strcmp(ext, "ciso") == 0) {
            out->experimental = 1;
        }
    }
    return rc;
}

int sigil_extract_from_path(const char *path, sigil_platform hint,
                            const sigil_options *opts, sigil_result *out) {
    if (!path || !out) return SIGIL_ERR_INVALID_ARG;

    sigil_platform resolved = hint;
    if (resolved == SIGIL_PLATFORM_AUTO) {
        resolved = sniff_from_extension(path_basename(path));
    }

    /* An archive extension names the container, never the console, so the
     * platform has to be resolved a second time from the member inside it.
     * That also makes this branch platform-agnostic: a zipped .nsp or .wbfs
     * takes the same route as a zipped Xbox image.
     *
     * PS Vita is excluded because its id comes from the archive's own file
     * name and the contents are never opened, so routing it through the
     * container reader would look inside for something that is not there.
     * Every other failure falls through rather than returning, leaving the
     * filename scanner its chance at an archive sigil cannot read. */
    if (resolved != SIGIL_PLATFORM_PSVITA && path_is_archive(path_basename(path))) {
        char inner[512];
        inner[0] = '\0';
        sigil_io *aio = sigil_io_open_zip(path, inner, sizeof(inner));
        if (aio) {
            sigil_platform inner_p = (hint != SIGIL_PLATFORM_AUTO)
                                   ? hint : sniff_from_extension(inner);
            if (inner_p != SIGIL_PLATFORM_AUTO) {
                int arc = sigil_extract_from_io(aio, inner, inner_p, opts, out);
                sigil_io_close(aio);
                if (arc == SIGIL_OK) return arc;
            } else {
                sigil_io_close(aio);
            }
        }
    }

    /* Vita dumps keep their identifier in sce_sys/param.sfo under a directory
     * named for the title, so the member is addressed by path suffix rather
     * than by the largest-member rule the generic archive branch uses.
     *
     * The probe also runs without a hint, because a .zip names no platform and
     * the filename scanner would otherwise claim a Vita serial for PSP: both
     * use the same bracket pattern. Finding that member is what identifies the
     * dump, so detection is by content rather than by name. */
    if (resolved == SIGIL_PLATFORM_PSVITA || resolved == SIGIL_PLATFORM_AUTO) {
        sigil_io *vio = NULL;
        if (path_is_archive(path_basename(path))) {
            vio = sigil_io_open_zip_member(path, VITA_SFO_MEMBER);
        } else if (resolved == SIGIL_PLATFORM_PSVITA) {
            char found[1024];
            if (path_is_directory(path)) {
                if (find_file_in_dir(path, "param.sfo", found, sizeof(found), 0) == SIGIL_OK) {
                    vio = sigil_io_open_file(found);
                }
            } else {
                vio = sigil_io_open_file(path);
            }
        }

        if (vio) {
            sigil_result vr;
            int vrc = sigil_extract_psvita(vio, path_basename(path), opts, &vr);
            sigil_io_close(vio);
            /* Only a binary hit settles it here. A filename-sourced result
             * would be the very guess this path exists to avoid, and with no
             * hint it could belong to another platform entirely. */
            if (vrc == SIGIL_OK && vr.source == SIGIL_SOURCE_BINARY) {
                *out = vr;
                return SIGIL_OK;
            }
        }
        if (resolved == SIGIL_PLATFORM_PSVITA) {
            return sigil_extract_psvita(NULL, path_basename(path), opts, out);
        }
    }

    if (resolved == SIGIL_PLATFORM_AUTO) {
        if (sigil_filename_fallback(path_basename(path), SIGIL_PLATFORM_AUTO, out) == SIGIL_OK) {
            return SIGIL_OK;
        }
        return SIGIL_ERR_UNKNOWN_PLATFORM;
    }

    char resolved_path[1024];
    const char *effective_path = path;
    const char *dir_target = directory_target_for_platform(resolved);
    if (dir_target && path_is_directory(path)) {
        if (find_file_in_dir(path, dir_target, resolved_path, sizeof(resolved_path), 0) != SIGIL_OK) {
            return SIGIL_ERR_NOT_FOUND;
        }
        effective_path = resolved_path;
    }

    sigil_io *io = open_io_for_platform(effective_path, resolved);
    if (!io) return SIGIL_ERR_IO;

    int rc = sigil_extract_from_io(io, path_basename(effective_path), resolved, opts, out);
    sigil_io_close(io);
    return rc;
}
