// SPDX-License-Identifier: MPL-2.0
/* A console's hard drive as an emulator keeps it in one file: a raw image, or
 * qcow2 as qemu writes it. The qcow2 layout is xemu's own copy of the format
 * (xemu-project/xemu at v0.8.136: docs/interop/qcow2.rst, with the masks,
 * flags and bounds of block/qcow2.h). Reads go through the
 * file's sigil_io. Writes are staged in memory and change nothing until the
 * caller takes them, as the whole file (sigil_disk_file) or as the parts
 * that changed (sigil_disk_runs), so a restore that fails before then
 * leaves the file as it was.
 *
 * Ported from CabinetOS's frontend/src/xboxhdd.cpp (MIT; the notice is in
 * licenses/cabinetos-MIT.txt). */
#ifndef SIGIL_DISK_IMAGE_H
#define SIGIL_DISK_IMAGE_H

#include "sigil_internal.h"

typedef struct sigil_disk sigil_disk;

/**
 * Opens the image behind `io`, which stays the caller's and must outlive the
 * disk. A file that starts with qcow2's magic is read as qcow2, any other as
 * a raw image.
 *
 * What is read and written: qcow2 version 2 or 3, zlib compression, 16-bit
 * reference counts. SIGIL_ERR_UNSUPPORTED_FORMAT for an image outside that,
 * which this code couldn't keep consistent: a backing file, encryption,
 * internal snapshots, an external data file, extended L2 entries, another
 * compression type or reference count width, dirty bitmaps or any other
 * autoclear feature bit, or one qemu left marked dirty.
 * SIGIL_ERR_DAMAGED for a qcow2 file whose header or tables don't hold
 * together, or one qemu marked corrupt.
 */
int  sigil_disk_open(const sigil_io *io, sigil_disk **out);
void sigil_disk_close(sigil_disk *d);

/** The drive's size as the console sees it. */
uint64_t sigil_disk_size(const sigil_disk *d);

/** Reads the drive at `off`, staged writes included. Unallocated space reads as zeros. */
int sigil_disk_read(sigil_disk *d, uint64_t off, void *buf, size_t len);

/**
 * Stages a write to the drive at `off`. On qcow2 a cluster that is
 * compressed, unallocated or shared is replaced by a new one at the end of
 * the file, counted in the reference counts, and the one it replaces is
 * released. Bytes the file already holds are not staged.
 */
int sigil_disk_write(sigil_disk *d, uint64_t off, const void *buf, size_t len);

/** A write is staged. */
bool sigil_disk_changed(const sigil_disk *d);

/* The largest image sigil_disk_file hands back. It holds the file in memory,
 * and a request for more than a device has can end the process holding it
 * instead of failing, so a larger image is refused. */
#define SIGIL_DISK_MAX_FILE (2ull * 1024u * 1024u * 1024u)

/**
 * The whole file with the staged writes applied; `*out` is malloc'd.
 * SIGIL_ERR_OOM for a file over SIGIL_DISK_MAX_FILE.
 */
int sigil_disk_file(sigil_disk *d, uint8_t **out, size_t *len);

/** Lets go of the file's sigil_io, which the caller may then close. Only sigil_disk_runs works after it. */
void sigil_disk_detach(sigil_disk *d);

/** One run of the file's bytes as they will be; return SIGIL_OK to go on. */
typedef int (*sigil_disk_run_fn)(void *ctx, uint64_t off, const uint8_t *data, size_t len);

/**
 * Calls `each` with every part of the file the staged writes change, as
 * runs of at most 1 MiB: first everything past the file's present end, in
 * order, which nothing in the file refers to until the rest is written,
 * then the changed pages inside it, in order. Stops at the first run `each`
 * refuses and returns its code.
 */
int sigil_disk_runs(sigil_disk *d, sigil_disk_run_fn each, void *ctx);

#endif
