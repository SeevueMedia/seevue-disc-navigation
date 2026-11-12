/* The Seevue read service, worker side. See readsvc.h. */
#define _DEFAULT_SOURCE
#include "readsvc.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ANSWERS_FD 3
#define REQUESTS_FD 4
#define CHUNK RS_MAX_READ
#define SLOTS 8
/* Handle 0 in the cache is the image. */
#define IMAGE_HANDLE 0u

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int broken;          /* the service ended: every call fails */
static int known;           /* info answered */
static int layout;
static uint64_t image_size;

/* The chunk cache: (handle, chunk index) -> up to CHUNK bytes. */
static struct {
  int used;
  uint32_t handle;
  uint64_t index;
  uint32_t len;
  uint64_t age;
  uint8_t data[CHUNK];
} slots[SLOTS];
static uint64_t clock_;

/* ---------- transport (called with the lock held) ---------- */

static int write_all(const void *p, size_t n) {
  const uint8_t *b = p;
  while (n > 0) {
    ssize_t w = write(REQUESTS_FD, b, n);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) return -1;
    b += w;
    n -= (size_t)w;
  }
  return 0;
}

static int read_all(void *p, size_t n) {
  uint8_t *b = p;
  while (n > 0) {
    ssize_t r = read(ANSWERS_FD, b, n);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return -1;
    b += r;
    n -= (size_t)r;
  }
  return 0;
}

static void put32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static void put64(uint8_t *p, uint64_t v) {
  for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t get64(const uint8_t *p) {
  return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}

/* Sends one request and reads its answer's status. The body is then read
   with read_body (exactly body_len bytes). Returns the status, or -1 when
   the service is gone (the worker cannot read on). */
static int request(char type, const void *payload, uint32_t len, uint32_t *body_len) {
  if (broken) return -1;
  uint8_t head[5];
  put32(head, len + 1);
  head[4] = (uint8_t)type;
  uint8_t answer[6];
  if (write_all(head, 5) < 0 || (len && write_all(payload, len) < 0) || read_all(answer, 6) < 0 ||
      answer[4] != 'A' || get32(answer) < 2) {
    broken = 1;
    return -1;
  }
  *body_len = get32(answer) - 2;
  return answer[5];
}

static int read_body(void *buf, uint32_t len) {
  if (read_all(buf, len) < 0) {
    broken = 1;
    return -1;
  }
  return 0;
}

/* Reads and drops a body that does not fit what was asked. */
static int unexpected(uint32_t len) {
  uint8_t sink[512];
  while (len > 0) {
    uint32_t n = len < sizeof sink ? len : (uint32_t)sizeof sink;
    if (read_body(sink, n) < 0) return -1;
    len -= n;
  }
  broken = 1; /* the server broke the protocol: stop here */
  return -1;
}

static int load_info(void) {
  if (known) return 0;
  uint32_t n;
  int st = request('I', NULL, 0, &n);
  if (st != RS_OK || n != 9) return st < 0 ? -1 : unexpected(n);
  uint8_t body[9];
  if (read_body(body, 9) < 0) return -1;
  layout = body[0];
  image_size = get64(body + 1);
  known = 1;
  return 0;
}

/* ---------- chunk cache (lock held) ---------- */

static void forget(uint32_t handle) {
  for (int i = 0; i < SLOTS; i++)
    if (slots[i].used && slots[i].handle == handle) slots[i].used = 0;
}

/* The chunk `index` of `handle` (whose size is `size`), fetched if needed. */
static int chunk(uint32_t handle, uint64_t size, uint64_t index) {
  for (int i = 0; i < SLOTS; i++) {
    if (slots[i].used && slots[i].handle == handle && slots[i].index == index) {
      slots[i].age = ++clock_;
      return i;
    }
  }
  /* A free slot, else the least recently used. */
  int victim = 0;
  for (int i = 0; i < SLOTS; i++) {
    if (!slots[i].used) {
      victim = i;
      break;
    }
    if (slots[i].age < slots[victim].age) victim = i;
  }
  uint64_t off = index * CHUNK;
  if (off >= size) return -1;
  uint32_t len = size - off < CHUNK ? (uint32_t)(size - off) : CHUNK;
  uint8_t p[16];
  uint32_t plen;
  char type;
  if (handle == IMAGE_HANDLE) {
    type = 'r';
    put64(p, off);
    put32(p + 8, len);
    plen = 12;
  } else {
    type = 'f';
    put32(p, handle);
    put64(p + 4, off);
    put32(p + 12, len);
    plen = 16;
  }
  uint32_t n;
  int st = request(type, p, plen, &n);
  if (st < 0) return -1;
  if (st != RS_OK) return n ? unexpected(n) : -1;
  if (n != len) return unexpected(n);
  slots[victim].used = 0;
  if (read_body(slots[victim].data, len) < 0) return -1;
  slots[victim].used = 1;
  slots[victim].handle = handle;
  slots[victim].index = index;
  slots[victim].len = len;
  slots[victim].age = ++clock_;
  return victim;
}

/* Copies up to len bytes at off of `handle` into buf. */
static int64_t cached_read(uint32_t handle, uint64_t size, uint64_t off, uint8_t *buf, size_t len) {
  if (off >= size || len == 0) return 0;
  if (len > size - off) len = (size_t)(size - off);
  size_t done = 0;
  while (done < len) {
    uint64_t at = off + done;
    int s = chunk(handle, size, at / CHUNK);
    if (s < 0) return done ? (int64_t)done : -1;
    uint32_t in = (uint32_t)(at % CHUNK);
    size_t n = slots[s].len - in;
    if (n > len - done) n = len - done;
    memcpy(buf + done, slots[s].data + in, n);
    done += n;
  }
  return (int64_t)done;
}

/* ---------- API ---------- */

int rs_info(int *out_layout, uint64_t *out_size) {
  pthread_mutex_lock(&lock);
  int r = load_info();
  if (r == 0) {
    *out_layout = layout;
    *out_size = image_size;
  }
  pthread_mutex_unlock(&lock);
  return r;
}

int64_t rs_read(uint64_t off, void *buf, size_t len) {
  pthread_mutex_lock(&lock);
  int64_t r = -1;
  if (load_info() == 0 && layout == RS_IMAGE) r = cached_read(IMAGE_HANDLE, image_size, off, buf, len);
  pthread_mutex_unlock(&lock);
  return r;
}

/* A path request ('s', 'l', 'o'): the status, the body length in *n. */
static int path_request(char type, const char *rel, uint32_t *n) {
  size_t len = strlen(rel);
  if (len > RS_MAX_PATH) return -1;
  return request(type, rel, (uint32_t)len, n);
}

int rs_stat(const char *rel, int *is_dir, uint64_t *size) {
  pthread_mutex_lock(&lock);
  uint32_t n;
  int st = path_request('s', rel, &n);
  int r = -1;
  if (st == RS_OK && n == 9) {
    uint8_t body[9];
    if (read_body(body, 9) == 0) {
      *is_dir = body[0] == 2;
      *size = get64(body + 1);
      r = RS_OK;
    }
  } else if (st == RS_MISSING && n == 0) {
    r = RS_MISSING;
  } else if (st >= 0 && n) {
    unexpected(n);
  }
  pthread_mutex_unlock(&lock);
  return r;
}

int rs_open(const char *rel, rs_file *file) {
  pthread_mutex_lock(&lock);
  uint32_t n;
  int st = path_request('o', rel, &n);
  int r = -1;
  if (st == RS_OK && n == 12) {
    uint8_t body[12];
    if (read_body(body, 12) == 0) {
      file->handle = get32(body);
      file->size = get64(body + 4);
      file->pos = 0;
      /* A reused handle number must not hit an old file's chunks. */
      forget(file->handle);
      r = file->handle == IMAGE_HANDLE ? -1 : RS_OK;
    }
  } else if (st == RS_MISSING && n == 0) {
    r = RS_MISSING;
  } else if (st >= 0 && n) {
    unexpected(n);
  }
  pthread_mutex_unlock(&lock);
  return r;
}

int64_t rs_fread(rs_file *file, void *buf, size_t len) {
  pthread_mutex_lock(&lock);
  int64_t r = cached_read(file->handle, file->size, file->pos, buf, len);
  if (r > 0) file->pos += (uint64_t)r;
  pthread_mutex_unlock(&lock);
  return r;
}

void rs_close(rs_file *file) {
  pthread_mutex_lock(&lock);
  forget(file->handle);
  uint8_t p[4];
  put32(p, file->handle);
  uint32_t n;
  int st = request('c', p, 4, &n);
  if (st >= 0 && n) unexpected(n);
  pthread_mutex_unlock(&lock);
}

int rs_list_open(const char *rel, rs_list *list) {
  pthread_mutex_lock(&lock);
  uint32_t n;
  int st = path_request('l', rel, &n);
  int r = -1;
  if (st == RS_OK) {
    list->body = malloc(n ? n : 1);
    list->len = n;
    list->at = 0;
    if (!list->body) {
      unexpected(n);
    } else if (read_body(list->body, n) == 0) {
      r = RS_OK;
    } else {
      free(list->body);
      list->body = NULL;
    }
  } else if (st == RS_MISSING && n == 0) {
    r = RS_MISSING;
  } else if (st >= 0 && n) {
    unexpected(n);
  }
  pthread_mutex_unlock(&lock);
  return r;
}

int rs_list_next(rs_list *list, char *out, size_t size) {
  if (list->at + 2 > list->len) return 1;
  size_t n = (size_t)list->body[list->at] | ((size_t)list->body[list->at + 1] << 8);
  if (list->at + 2 + n > list->len || n + 1 > size) return -1;
  memcpy(out, list->body + list->at + 2, n);
  out[n] = 0;
  list->at += 2 + n;
  return 0;
}

void rs_list_free(rs_list *list) {
  free(list->body);
  list->body = NULL;
}

int rs_relative(const char *root, const char *path, char *out, size_t size) {
  size_t rl = strlen(root);
  if (strncmp(path, root, rl) != 0) return -1;
  const char *p = path + rl;
  if (rl && *p && *p != '/') return -1; /* "/disc2" is not under "/disc" */
  size_t o = 0;
  while (*p) {
    while (*p == '/') p++;
    if (!*p) break;
    if (o) {
      if (o + 1 >= size) return -1;
      out[o++] = '/';
    }
    while (*p && *p != '/') {
      if (o + 1 >= size) return -1;
      out[o++] = *p++;
    }
  }
  out[o] = 0;
  return 0;
}
