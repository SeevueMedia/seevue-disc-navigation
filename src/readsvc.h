/*
 * The Seevue read service, worker side (protocol 2).
 *
 * The workers never open the disc themselves: they ask the server, which
 * answers from its source I/O engine, so a disc on any source (a direct SMB
 * share included) plays the same way. Answers arrive on fd 3, requests leave
 * on fd 4, framed like the control protocol: u32 little-endian length of
 * (type + payload), u8 type, payload.
 *
 *   'I' info                      -> u8 layout (1 image, 2 folder), u64 image size
 *   'r' read  u64 off, u32 len    -> bytes of the image
 *   's' stat  path                -> u8 kind (1 file, 2 folder), u64 size
 *   'l' list  path                -> per entry: u16 length, name
 *   'o' open  path                -> u32 handle, u64 size
 *   'f' read  u32 h, u64 off, u32 len -> bytes of the file
 *   'c' close u32 h               -> nothing
 *
 * Every answer is one 'A' frame: u8 status (0 ok, 1 missing, 2 failed,
 * 3 too large), then the body. Paths are relative to the disc root, '/'
 * separated, with no '.', '..' or empty component; the server ends a worker
 * that sends anything else, reads outside a file, or floods it.
 *
 * Every call is serialised by one lock (libbluray's BD-J threads read too)
 * and goes through a small cache of 64 KiB chunks, so sequential reads cost
 * one round trip per chunk.
 *
 * Licence: GPL-2.0-or-later, like the rest of this repository.
 */
#ifndef SEEVUE_READSVC_H
#define SEEVUE_READSVC_H

#include <stddef.h>
#include <stdint.h>

#define RS_MAX_READ 65536u
#define RS_MAX_PATH 1024

enum { RS_OK = 0, RS_MISSING = 1, RS_FAILED = 2, RS_TOO_LARGE = 3 };
enum { RS_IMAGE = 1, RS_FOLDER = 2 };

/* The disc's layout and, for an image, its size. 0, or -1 without a service. */
int rs_info(int *layout, uint64_t *size);

/* Reads the image: up to len bytes at off, fewer at its end. Returns the
   bytes read, 0 at the end, -1 on failure. */
int64_t rs_read(uint64_t off, void *buf, size_t len);

/* A disc file opened through the service. */
typedef struct {
  uint32_t handle;
  uint64_t size;
  uint64_t pos;
} rs_file;

/* RS_OK with *is_dir and *size, RS_MISSING, or -1. */
int rs_stat(const char *rel, int *is_dir, uint64_t *size);
/* RS_OK with *file filled, RS_MISSING, or -1. */
int rs_open(const char *rel, rs_file *file);
/* Reads from file->pos and advances it; fewer at the end. Bytes, 0, or -1. */
int64_t rs_fread(rs_file *file, void *buf, size_t len);
void rs_close(rs_file *file);

/* A folder listing: names, read one by one. */
typedef struct {
  uint8_t *body;
  size_t len, at;
} rs_list;
/* RS_OK with *list filled (free with rs_list_free), RS_MISSING, or -1. */
int rs_list_open(const char *rel, rs_list *list);
/* 0 with the next name in out, 1 at the end, -1 if the name does not fit. */
int rs_list_next(rs_list *list, char *out, size_t size);
void rs_list_free(rs_list *list);

/* A library path as the service's relative path: root (a prefix such as
   "/disc", or "") removed, separators collapsed, no leading or trailing '/'.
   0, or -1 when path is not under root or does not fit. */
int rs_relative(const char *root, const char *path, char *out, size_t size);

#endif
