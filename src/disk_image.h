// SPDX-License-Identifier: MPL-2.0
/* A console's hard drive as an emulator keeps it in one file: a raw image, or
 * qcow2 as qemu writes it. The qcow2 layout is xemu's own copy of the format
 * (xemu-project/xemu at v0.8.136: docs/interop/qcow2.rst, with the masks,
 * flags and bounds of block/qcow2.h). Reads go through the
 * file's sigil_io. Writes are staged in memory and change nothing until the
 * caller takes them, as the whole file (sigil_disk_file) or as the parts
 * that changed (sigil_disk_runs), in an order that keeps the image
 * consistent between any two of them.
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
 * Stages a write to the drive at `off`, in the present phase. On qcow2 a
 * cluster that is compressed, unallocated or shared is replaced by a new one
 * at the end of the file, counted in the reference counts, and the one it
 * replaces is released. Bytes the file already holds are not staged.
 */
int sigil_disk_write(sigil_disk *d, uint64_t off, const void *buf, size_t len);

/** A write is staged. */
bool sigil_disk_changed(const sigil_disk *d);

/* How many phases a drive's writes can be put in. */
#define SIGIL_DISK_PHASES 4u

/**
 * Puts the writes staged from now on in phase `phase`. Writes start in phase
 * 0 and never go back. Written in parts (sigil_disk_runs), every write of a
 * phase reaches the file, with the qcow2 table entries that map it, before
 * any write of a later phase does, so a file system that stages its writes
 * in phases keeps that order on the file. SIGIL_ERR_INVALID_ARG for a phase
 * before the present one, or past the last.
 */
int sigil_disk_set_phase(sigil_disk *d, unsigned phase);

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

/** One run of the file's bytes; return SIGIL_OK to go on. */
typedef int (*sigil_disk_run_fn)(void *ctx, uint64_t off, const uint8_t *data, size_t len);

/**
 * Calls `each` with the parts of the file the staged writes change, as runs
 * of at most 1 MiB, and stops at the first run `each` refuses, returning its
 * code. `in_steps` gives the order that leaves the image consistent after
 * any run, with clusters counted that nothing refers to as its only waste:
 * first what the file gains past its present end, which nothing in it refers
 * to yet; then the counts new clusters gain; then for each phase its bytes,
 * then the L1 and L2 entries that map them; and last the counts replaced
 * clusters lose. A page more than one step changes goes out once for each,
 * as that step leaves it, and no run holds two steps. Without `in_steps`,
 * each changed part goes out once, as it ends up, for reading the file back.
 */
int sigil_disk_runs(sigil_disk *d, bool in_steps, sigil_disk_run_fn each, void *ctx);

#endif
