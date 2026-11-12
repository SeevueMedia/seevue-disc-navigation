/*
 * bdnavd: Blu-ray navigation worker (libbluray, HDMV and BD-J menus).
 *
 * Runs libbluray's navigation over a decrypted Blu-ray image for one viewing
 * session, under a supervising server. The image arrives as an open
 * file descriptor (fd 3) and is opened through /dev/fd/3.
 *
 * Licence: GPL-2.0-or-later; links libbluray and libudfread (LGPL-2.1-or-later).
 *
 * Protocol: navd's framing (u32 LE length of type+payload, u8 type, payload).
 * Every command produces zero or more 'D'/'E'/'G' frames, then one 'R'.
 *
 *   Commands (server -> bdnavd)
 *     'O' open    u8 region (1 A, 2 B, 4 C), then "eng" style 3-letter audio,
 *                 menu and subtitle languages and a 2-letter country (11 bytes)
 *     'N' next    u32 max_units: read up to max_units 6144-byte aligned units;
 *                 stops early at a playlist start, discontinuity, seek, still,
 *                 idle, end of title or error
 *     'K' key     u32 libbluray key code, i64 pts (90 kHz, -1 unknown)
 *     'M' menu    i64 pts: top menu call
 *     'C' chapter u32 chapter (0-based) in the current playlist
 *     'A' audio   u32 stream (1-based, the play item's STN order)
 *     'S' subtitles u32 stream (1-based), u8 enable (0 hides them)
 *     'T' seek    i64 90 kHz time in the current playlist
 *     'G' graphics poll: hands over BD-J plane changes (also sent before every reply)
 *   'O' may carry a disc folder's path after its 12 bytes of settings. BD-J needs
 *   JAVA_HOME and LIBBLURAY_CP; BDNAVD_STORAGE holds its persistent and cache data.
 *   Discs with BD-J titles and no usable Java VM open in direct mode: the main
 *   title plays without the disc's menus (keys other than chapters, audio and
 *   subtitles are refused).
 *     'X' quit
 *   Replies (bdnavd -> server)
 *     'D' data    whole 192-byte source packets (aligned 6144-byte units, except
 *                 where libbluray returns a short read at a clip's end)
 *     'E' event   one JSON object
 *     'G' graphics one plane update: u8 plane (0 PG, 1 IG), u8 flags (1 cleared
 *                 first), i64 pts (-1: none), u16 x, y, w, h, then w*h RGBA
 *                 pixels: the plane's content in that rectangle after the update
 *     'R' reply   one JSON object: {"ok":true,...} or {"ok":false,"error":"..."}
 */
#define _DARWIN_C_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <sys/stat.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libbluray/bluray.h>
#include <libbluray/keys.h>
#include <libbluray/overlay.h>
#include <libbluray/player_settings.h>
#include "bdcaps.h"

#define UNIT 6144
#define MAX_UNITS 64           /* per 'N' command and per 'D' frame */
#define JSON_MAX 16384
#define PLANE_W 1920
#define PLANE_H 1080

static BLURAY *bd;
/* This worker's own BD-J cache and Java temp directories, removed on exit. */
static char own_cache[1024], own_tmp[1024];

static int rm_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw) {
  (void)st; (void)flag; (void)ftw;
  return remove(path);
}

static void remove_tree(const char *dir) {
  if (*dir) nftw(dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}
static uint8_t batch[MAX_UNITS * UNIT];
static uint32_t batch_len;
static uint8_t readbuf[MAX_UNITS * UNIT];

static uint32_t *plane[2];
static int bbox[2][4];
static int cleared[2];
static uint32_t cur_playlist = 0xffffffff, cur_audio = 0, cur_pg = 0, pg_on = 0;
/* Direct mode: the disc's menus cannot run (BD-J without a Java VM): the main
   title plays without navigation, as players offer "play main title". */
static int direct;

/* ---------- framing ---------- */

/* The protocol's output: a private copy of stdout, because the JVM (BD-J) and
   libbluray print to stdout, which is pointed at stderr instead. */
static int proto_out = STDOUT_FILENO;
/* Held for each whole frame: the wake notice is written from BD-J's threads. */
static pthread_mutex_t out_lock = PTHREAD_MUTEX_INITIALIZER;

static int write_all(const void *p, size_t n) {
  const uint8_t *b = p;
  while (n > 0) {
    ssize_t w = write(proto_out, b, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    b += w;
    n -= (size_t)w;
  }
  return 0;
}

/* Starts a frame; frame_done() ends it. */
static void frame_head(char type, uint32_t len) {
  pthread_mutex_lock(&out_lock);
  uint8_t head[5];
  uint32_t total = len + 1;
  head[0] = total & 0xff;
  head[1] = (total >> 8) & 0xff;
  head[2] = (total >> 16) & 0xff;
  head[3] = (total >> 24) & 0xff;
  head[4] = (uint8_t)type;
  if (write_all(head, 5) < 0) _exit(3);
}

static void frame_done(void) { pthread_mutex_unlock(&out_lock); }

static void frame(char type, const void *payload, uint32_t len) {
  frame_head(type, len);
  if (len > 0 && write_all(payload, len) < 0) _exit(3);
  frame_done();
}

#ifndef SEEVUE_WORKER_VERSION
#define SEEVUE_WORKER_VERSION "dev"
#endif
/* Protocol v1: the hello, once, before any command is read. */
static void send_hello(void) {
  static const char hello[] = "{\"protocol\":1,\"worker\":\"bdnavd\",\"version\":\"" SEEVUE_WORKER_VERSION "\"}";
  frame('H', hello, (uint32_t)(sizeof hello - 1));
}

static int read_all(void *p, size_t n) {
  uint8_t *b = p;
  while (n > 0) {
    ssize_t r = read(STDIN_FILENO, b, n);
    if (r == 0) return -1;
    if (r < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    b += r;
    n -= (size_t)r;
  }
  return 0;
}

static char json[JSON_MAX];
static size_t json_len;
static int json_overflow;

static void j(const char *fmt, ...) {
  if (json_overflow) return;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(json + json_len, JSON_MAX - json_len, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= JSON_MAX - json_len) {
    json_overflow = 1;
    return;
  }
  json_len += (size_t)n;
}

static void j_begin(void) {
  json_len = 0;
  json_overflow = 0;
  j("{");
}

static void argb_send_pending(void);

static void j_end(char type) {
  /* BD-J draws from Java threads: hand over its plane changes before each reply. */
  if (type == 'R') argb_send_pending();
  if (json_overflow) {
    j_begin();
    j("\"ok\":false,\"error\":\"json_overflow\"");
  }
  j("}");
  frame(type, json, (uint32_t)json_len);
}

static void reply_error(const char *what) {
  j_begin();
  j("\"ok\":false,\"error\":\"%s\"", what);
  j_end('R');
}

static void flush_batch(void) {
  if (batch_len > 0) {
    frame('D', batch, batch_len);
    batch_len = 0;
  }
}

static void j_position(void) {
  j("\"title\":%u,\"playlist\":%d,\"chapter\":%u,\"time_90k\":%" PRIu64 ",\"audio_sel\":%u,\"pg_sel\":%u,\"pg_on\":%u", bd_get_current_title(bd),
    cur_playlist == 0xffffffff ? -1 : (int)cur_playlist, bd_get_current_chapter(bd), bd_tell_time(bd), cur_audio, cur_pg, pg_on);
}

/* ---------- overlays ---------- */

static void grow(int p, int x, int y, int w, int h) {
  int *b = bbox[p];
  if (b[2] <= b[0]) {
    b[0] = x; b[1] = y; b[2] = x + w; b[3] = y + h;
  } else {
    if (x < b[0]) b[0] = x;
    if (y < b[1]) b[1] = y;
    if (x + w > b[2]) b[2] = x + w;
    if (y + h > b[3]) b[3] = y + h;
  }
  if (b[2] > PLANE_W) b[2] = PLANE_W;
  if (b[3] > PLANE_H) b[3] = PLANE_H;
}

/* BT.709 limited-range YCrCb to RGBA bytes in memory order (R, G, B, A). */
static uint32_t rgba(const BD_PG_PALETTE_ENTRY *e) {
  double y = 1.164 * (e->Y - 16), cr = e->Cr - 128, cb = e->Cb - 128;
  double r = y + 1.793 * cr, g = y - 0.213 * cb - 0.533 * cr, b = y + 2.112 * cb;
  int R = r < 0 ? 0 : r > 255 ? 255 : (int)r, G = g < 0 ? 0 : g > 255 ? 255 : (int)g,
      B = b < 0 ? 0 : b > 255 ? 255 : (int)b;
  return (uint32_t)R | ((uint32_t)G << 8) | ((uint32_t)B << 16) | ((uint32_t)e->T << 24);
}

static void put16(uint8_t *p, uint16_t v) { p[0] = v & 0xff; p[1] = v >> 8; }

static void send_plane(int p, int64_t pts) {
  int *b = bbox[p];
  int w = b[2] - b[0], h = b[3] - b[1];
  if (w <= 0 || h <= 0) {
    if (!cleared[p]) return;
    w = 0; h = 0;
  }
  flush_batch();
  uint8_t head[18];
  if (pts <= 0) pts = -1; /* libbluray uses 0 for "no time"; stream times are never 0 */
  head[0] = (uint8_t)p;
  head[1] = cleared[p] ? 1 : 0;
  for (int i = 0; i < 8; i++) head[2 + i] = (uint8_t)(((uint64_t)pts >> (8 * i)) & 0xff);
  put16(head + 10, (uint16_t)(w ? b[0] : 0));
  put16(head + 12, (uint16_t)(w ? b[1] : 0));
  put16(head + 14, (uint16_t)w);
  put16(head + 16, (uint16_t)h);
  frame_head('G', 18 + (uint32_t)w * (uint32_t)h * 4);
  if (write_all(head, 18) < 0) _exit(3);
  for (int y = 0; y < h; y++)
    if (write_all(plane[p] + (size_t)(b[1] + y) * PLANE_W + b[0], (size_t)w * 4) < 0) _exit(3);
  frame_done();
  memset(bbox[p], 0, sizeof bbox[p]);
  cleared[p] = 0;
}

static void overlay(void *handle, const BD_OVERLAY *ov) {
  (void)handle;
  if (!ov) return;
  int p = ov->plane == BD_OVERLAY_IG ? 1 : ov->plane == BD_OVERLAY_PG ? 0 : -1;
  if (p < 0) return;
  if (!plane[p]) plane[p] = calloc((size_t)PLANE_W * PLANE_H, 4);
  if (!plane[p]) return;
  switch (ov->cmd) {
    case BD_OVERLAY_INIT:
    case BD_OVERLAY_CLEAR:
    case BD_OVERLAY_HIDE:
    case BD_OVERLAY_CLOSE:
      memset(plane[p], 0, (size_t)PLANE_W * PLANE_H * 4);
      memset(bbox[p], 0, sizeof bbox[p]);
      cleared[p] = 1;
      /* HIDE follows a flushed clear (already sent with its time); CLOSE applies now. */
      if (ov->cmd == BD_OVERLAY_CLOSE) send_plane(p, -1);
      break;
    case BD_OVERLAY_WIPE:
      for (int y = ov->y; y < ov->y + ov->h && y < PLANE_H; y++)
        for (int x = ov->x; x < ov->x + ov->w && x < PLANE_W; x++) plane[p][y * PLANE_W + x] = 0;
      grow(p, ov->x, ov->y, ov->w, ov->h);
      break;
    case BD_OVERLAY_DRAW: {
      uint32_t pal[256];
      for (int i = 0; i < 256; i++) pal[i] = ov->palette ? rgba(&ov->palette[i]) : 0;
      if (ov->img) {
        const BD_PG_RLE_ELEM *e = ov->img;
        int x = 0, y = 0;
        while (y < ov->h) {
          if (e->len == 0) { x = 0; y++; e++; continue; }
          for (int i = 0; i < e->len && x < ov->w; i++, x++) {
            int px = ov->x + x, py = ov->y + y;
            if (px < PLANE_W && py < PLANE_H) plane[p][py * PLANE_W + px] = pal[e->color];
          }
          e++;
        }
      }
      grow(p, ov->x, ov->y, ov->w, ov->h);
      break;
    }
    case BD_OVERLAY_FLUSH:
      send_plane(p, ov->pts);
      break;
  }
}

/* ---------- BD-J graphics (ARGB, drawn by libbluray from Java threads) ---------- */

static pthread_mutex_t argb_lock = PTHREAD_MUTEX_INITIALIZER;   /* the planes' pixels */
static pthread_mutex_t argb_state = PTHREAD_MUTEX_INITIALIZER;  /* pending changes */
static BD_ARGB_BUFFER argb;
static uint32_t *argb_shadow[3];   /* last state handed over, to send only changed pixels */
static int argb_dirty[3][4];       /* pending changed area, x0 y0 x1 y1 exclusive */
static int argb_closed[3];
static int argb_flushed;          /* BD-J has drawn since its title started */
static atomic_int argb_woken;      /* a wake notice is out for changes not yet sent */

/* A BD-J title that has drawn nothing BDJ_RETRY_MS after starting gets a menu
   call (as the remote's top-menu key would), at most BDJ_RETRIES times per
   start. Ratatouille's application failed to start intermittently in
   libbluray's BD-J ("failed: Addendum.png", an image load on an intact cached
   file; more often while the stream is being read); a menu call restarted it
   every time in testing (results/c9). */
#define BDJ_RETRY_MS 5000
#define BDJ_RETRIES 2
static uint64_t bdj_started_at;
static int bdj_retries;
static uint64_t bdj_bytes;        /* stream read since the BD-J title started */

/* The failure itself is only in libbluray's log ("doStart() failed"): a title
   whose main application failed can still draw (Ratatouille's loading
   flourish), so "drew nothing" misses it. The worker reads its own stderr
   (libbluray's and the JVM's log), passes it on, and restarts the title with
   a menu call when an application fails to start, at most BDJ_FAIL_RETRIES
   times per disc. */
#define BDJ_FAIL_RETRIES 3
static atomic_int bdj_start_failed;
/* An application that cannot even load ("doLoad() failed": its JAR is missing or
   unreadable) will not load on a restart: report it once instead. */
static atomic_int bdj_load_failed;
static char bdj_fail_line[256];   /* the log line, JSON-safe (written before the flag) */
static int bdj_reported;
static int bdj_fail_retries;
static int real_stderr = STDERR_FILENO;

static void *log_relay(void *arg) {
  int fd = (int)(intptr_t)arg;
  char buf[4096], line[512];
  size_t used = 0;
  for (;;) {
    ssize_t n = read(fd, buf, sizeof buf);
    if (n <= 0) break;
    for (ssize_t off = 0; off < n;) {
      ssize_t w = write(real_stderr, buf + off, (size_t)(n - off));
      if (w <= 0) break;
      off += w;
    }
    for (ssize_t i = 0; i < n; i++) {
      if (buf[i] == '\n' || used == sizeof line - 1) {
        line[used] = 0;
        int start = strstr(line, "doStart() failed") != NULL, load = strstr(line, "doLoad() failed") != NULL;
        if (start || load) {
          size_t k = 0;
          for (const char *c = line; *c && k + 1 < sizeof bdj_fail_line; c++)
            if (*c != '"' && *c != '\\' && (unsigned char)*c >= 0x20) bdj_fail_line[k++] = *c;
          bdj_fail_line[k] = 0;
          atomic_store(load ? &bdj_load_failed : &bdj_start_failed, 1);
        }
        used = 0;
      } else {
        line[used++] = buf[i];
      }
    }
  }
  return NULL;
}

static uint64_t mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
static uint8_t *argb_stage;

static void argb_lock_cb(BD_ARGB_BUFFER *b) { (void)b; pthread_mutex_lock(&argb_lock); }
static void argb_unlock_cb(BD_ARGB_BUFFER *b) { (void)b; pthread_mutex_unlock(&argb_lock); }

static void argb_overlay(void *handle, const BD_ARGB_OVERLAY *ov) {
  (void)handle;
  if (!ov || ov->plane > 2) return;
  int p = ov->plane;
  switch (ov->cmd) {
    case BD_ARGB_OVERLAY_INIT:
      pthread_mutex_lock(&argb_lock);
      if (!argb.buf[p]) argb.buf[p] = calloc((size_t)PLANE_W * PLANE_H, 4);
      if (!argb_shadow[p]) argb_shadow[p] = calloc((size_t)PLANE_W * PLANE_H, 4);
      argb.width = PLANE_W;
      argb.height = PLANE_H;
      pthread_mutex_unlock(&argb_lock);
      break;
    case BD_ARGB_OVERLAY_CLOSE:
      pthread_mutex_lock(&argb_state);
      argb_closed[p] = 1;
      pthread_mutex_unlock(&argb_state);
      break;
    case BD_ARGB_OVERLAY_FLUSH: {
      pthread_mutex_lock(&argb_lock);
      int x0 = argb.dirty[p].x0, y0 = argb.dirty[p].y0, x1 = argb.dirty[p].x1 + 1, y1 = argb.dirty[p].y1 + 1;
      pthread_mutex_unlock(&argb_lock);
      if (x1 <= x0 || y1 <= y0) break;
      pthread_mutex_lock(&argb_state);
      argb_flushed = 1;
      int *d = argb_dirty[p];
      if (d[2] <= d[0]) { d[0] = x0; d[1] = y0; d[2] = x1; d[3] = y1; }
      else {
        if (x0 < d[0]) d[0] = x0;
        if (y0 < d[1]) d[1] = y0;
        if (x1 > d[2]) d[2] = x1;
        if (y1 > d[3]) d[3] = y1;
      }
      pthread_mutex_unlock(&argb_state);
      /* Protocol v1 wake notice, once until the changes are sent, so the
         server asks for them (G) instead of polling. */
      if (!atomic_exchange(&argb_woken, 1)) {
        static const char wake[] = "{\"n\":\"graphics_changed\"}";
        frame('W', wake, (uint32_t)(sizeof wake - 1));
      }
      break;
    }
    default:
      break;
  }
}

static void argb_send_pending(void) {
  /* Only the menu plane (1) is shown; the background plane (2) would sit behind video. */
  atomic_store(&argb_woken, 0);
  for (int p = 0; p < 2; p++) {
    pthread_mutex_lock(&argb_state);
    int d[4] = {argb_dirty[p][0], argb_dirty[p][1], argb_dirty[p][2], argb_dirty[p][3]};
    int closed = argb_closed[p];
    memset(argb_dirty[p], 0, sizeof argb_dirty[p]);
    argb_closed[p] = 0;
    pthread_mutex_unlock(&argb_state);
    if (closed && argb_shadow[p]) {
      memset(argb_shadow[p], 0, (size_t)PLANE_W * PLANE_H * 4);
      flush_batch();
      uint8_t head[18] = {(uint8_t)p, 1};
      int64_t none = -1;
      memcpy(head + 2, &none, 8);
      frame('G', head, 18);
    }
    if (d[2] <= d[0] || !argb.buf[p] || !argb_shadow[p]) continue;
    if (d[2] > PLANE_W) d[2] = PLANE_W;
    if (d[3] > PLANE_H) d[3] = PLANE_H;
    pthread_mutex_lock(&argb_lock);
    /* Shrink to the pixels that changed since the last hand-over. */
    int cx0 = PLANE_W, cy0 = PLANE_H, cx1 = -1, cy1 = -1;
    for (int y = d[1]; y < d[3]; y++) {
      const uint32_t *a = argb.buf[p] + (size_t)y * PLANE_W, *b = argb_shadow[p] + (size_t)y * PLANE_W;
      if (!memcmp(a + d[0], b + d[0], (size_t)(d[2] - d[0]) * 4)) continue;
      int l = d[0], r = d[2] - 1;
      while (a[l] == b[l]) l++;
      while (a[r] == b[r]) r--;
      if (l < cx0) cx0 = l;
      if (r > cx1) cx1 = r;
      if (y < cy0) cy0 = y;
      cy1 = y;
    }
    if (cx1 < 0) { pthread_mutex_unlock(&argb_lock); continue; }
    int w = cx1 - cx0 + 1, h = cy1 - cy0 + 1;
    if (!argb_stage) argb_stage = malloc((size_t)PLANE_W * PLANE_H * 4);
    if (!argb_stage) { pthread_mutex_unlock(&argb_lock); continue; }
    for (int y = 0; y < h; y++) {
      uint32_t *a = argb.buf[p] + (size_t)(cy0 + y) * PLANE_W + cx0;
      memcpy(argb_shadow[p] + (size_t)(cy0 + y) * PLANE_W + cx0, a, (size_t)w * 4);
      uint8_t *o = argb_stage + (size_t)y * w * 4;
      for (int x = 0; x < w; x++) {
        uint32_t v = a[x];
        o[4 * x] = (uint8_t)(v >> 16);
        o[4 * x + 1] = (uint8_t)(v >> 8);
        o[4 * x + 2] = (uint8_t)v;
        o[4 * x + 3] = (uint8_t)(v >> 24);
      }
    }
    pthread_mutex_unlock(&argb_lock);
    flush_batch();
    uint8_t head[18] = {(uint8_t)p, 0};
    int64_t none = -1;
    memcpy(head + 2, &none, 8);
    put16(head + 10, (uint16_t)cx0);
    put16(head + 12, (uint16_t)cy0);
    put16(head + 14, (uint16_t)w);
    put16(head + 16, (uint16_t)h);
    frame_head('G', 18 + (uint32_t)w * (uint32_t)h * 4);
    if (write_all(head, 18) < 0 || write_all(argb_stage, (size_t)w * h * 4) < 0) _exit(3);
    frame_done();
  }
}

/* ---------- events ---------- */

static const char *ev_name(uint32_t e) {
  static const char *n[] = {"none", "error", "read_error", "encrypted", "angle", "title", "playlist",
                            "playitem", "chapter", "playmark", "end_of_title", "audio_stream", "ig_stream",
                            "pg_textst_stream", "pip_pg_textst_stream", "secondary_audio_stream",
                            "secondary_video_stream", "pg_textst", "pip_pg_textst", "secondary_audio",
                            "secondary_video", "secondary_video_size", "playlist_stop", "discontinuity",
                            "seek", "still", "still_time", "sound_effect", "idle", "popup", "menu",
                            "stereoscopic_status", "key_interest_table", "uo_mask_changed"};
  return e < sizeof n / sizeof n[0] ? n[e] : "unknown";
}

static void j_streams(uint32_t item) {
  BLURAY_TITLE_INFO *ti = cur_playlist == 0xffffffff ? NULL : bd_get_playlist_info(bd, cur_playlist, 0);
  if (!ti || item >= ti->clip_count) {
    j(",\"video\":[],\"audio\":[]");
    if (ti) bd_free_title_info(ti);
    return;
  }
  BLURAY_CLIP_INFO *c = &ti->clips[item];
  j(",\"clip\":\"%s\",\"video\":[", c->clip_id);
  for (int i = 0; i < c->video_stream_count; i++)
    j("%s{\"pid\":%u,\"coding\":%u}", i ? "," : "", c->video_streams[i].pid, c->video_streams[i].coding_type);
  j("],\"audio\":[");
  for (int i = 0; i < c->audio_stream_count; i++)
    j("%s{\"pid\":%u,\"coding\":%u,\"lang\":\"%.3s\"}", i ? "," : "", c->audio_streams[i].pid,
      c->audio_streams[i].coding_type, (const char *)c->audio_streams[i].lang);
  j("],\"pg\":[");
  for (int i = 0; i < c->pg_stream_count; i++)
    j("%s{\"pid\":%u,\"coding\":%u,\"lang\":\"%.3s\"}", i ? "," : "", c->pg_streams[i].pid,
      c->pg_streams[i].coding_type, (const char *)c->pg_streams[i].lang);
  j("]");
  bd_free_title_info(ti);
}

/* Emits one event; returns its stop reason, or NULL to keep reading. */
static const char *event(const BD_EVENT *ev) {
  if (ev->event == BD_EVENT_NONE) return NULL;
  switch (ev->event) {
    case BD_EVENT_PLAYLIST: cur_playlist = ev->param; break;
    case BD_EVENT_TITLE: {
      const BLURAY_DISC_INFO *di = bd_get_disc_info(bd);
      if (di && ev->param >= 1 && ev->param <= di->num_titles && di->titles[ev->param] &&
          di->titles[ev->param]->bdj) {
        pthread_mutex_lock(&argb_state);
        argb_flushed = 0;
        pthread_mutex_unlock(&argb_state);
        bdj_started_at = mono_ms();
        bdj_bytes = 0;
      }
      break;
    }
    case BD_EVENT_AUDIO_STREAM: cur_audio = ev->param; break;
    case BD_EVENT_PG_TEXTST_STREAM: cur_pg = ev->param; break;
    case BD_EVENT_PG_TEXTST: pg_on = ev->param; break;
    default: break;
  }
  flush_batch();
  j_begin();
  j("\"ev\":\"%s\",\"param\":%u,", ev_name(ev->event), ev->param);
  j_position();
  if (ev->event == BD_EVENT_PLAYITEM) j_streams(ev->param);
  j_end('E');
  switch (ev->event) {
    case BD_EVENT_ERROR: return "error";
    case BD_EVENT_ENCRYPTED: return "encrypted";
    case BD_EVENT_PLAYLIST: return "playlist";
    case BD_EVENT_DISCONTINUITY: return "discontinuity";
    case BD_EVENT_SEEK: return "seek";
    case BD_EVENT_END_OF_TITLE: return "end_of_title";
    case BD_EVENT_IDLE: return "idle";
    case BD_EVENT_STILL: return ev->param == 1 ? "still" : NULL;
    case BD_EVENT_STILL_TIME: return "still_time";
    default: return NULL;
  }
}

/* Called on every read and graphics poll. */
/* stage "restart" (a failed start, restarted from the disc's menu) is reported every
   time; "load" and "start" (restarts used up) once. */
static void bdj_report(const char *stage) {
  if (strcmp(stage, "restart")) {
    if (bdj_reported) return;
    bdj_reported = 1;
  }
  j_begin();
  j("\"ev\":\"bdj_failed\",\"stage\":\"%s\",\"detail\":\"%s\"", stage, bdj_fail_line);
  j_end('E');
}

static void bdj_watch(void) {
  if (atomic_exchange(&bdj_load_failed, 0)) {
    fprintf(stderr, "bdnavd: BD-J application failed to load (no restart)\n");
    bdj_report("load");
    return;
  }
  int start_failed = atomic_exchange(&bdj_start_failed, 0);
  if (start_failed && bdj_fail_retries >= BDJ_FAIL_RETRIES) { bdj_report("start"); return; }
  if (start_failed && bdj_fail_retries < BDJ_FAIL_RETRIES) {
    bdj_fail_retries++;
    fprintf(stderr, "bdnavd: BD-J application failed to start; menu call to restart it (%d/%d)\n",
            bdj_fail_retries, BDJ_FAIL_RETRIES);
    bdj_report("restart");
    bd_menu_call(bd, -1);
    return;
  }
  if (!bdj_started_at) return;
  pthread_mutex_lock(&argb_state);
  int drawn = argb_flushed;
  pthread_mutex_unlock(&argb_state);
  /* Drawing or playing video: the application is running (some play intros first). */
  if (drawn || bdj_bytes > 0) { bdj_started_at = 0; bdj_retries = 0; return; }
  if (mono_ms() - bdj_started_at < BDJ_RETRY_MS) return;
  if (bdj_retries >= BDJ_RETRIES) { bdj_started_at = 0; return; }
  bdj_retries++;
  fprintf(stderr, "bdnavd: BD-J title drew nothing in %d ms; menu call to restart it (%d/%d)\n",
          BDJ_RETRY_MS, bdj_retries, BDJ_RETRIES);
  bd_menu_call(bd, -1);
  bdj_started_at = mono_ms();
}

static void drain_events(void) {
  BD_EVENT ev;
  while (bd_get_event(bd, &ev)) event(&ev);
}

/* ---------- commands ---------- */

static void cmd_open(const uint8_t *p, uint32_t len) {
  if (bd) { reply_error("already_open"); return; }
  if (len < 12) { reply_error("bad_open"); return; }
  /* A disc folder (BDMV inside) comes as a path after the settings; an image as fd 3. */
  /* libbluray opens the image more than once (BD-J's file access has its own),
     and makes each seek + read atomic per opening only. /dev/fd/3 on macOS
     duplicates the descriptor, so all openings would share one file position
     and race (BD-J start-up failed with an I/O error): reopen by the file's own
     path instead. Linux's /proc/self/fd/3 already gives an independent opening. */
  char path[PATH_MAX > 1024 ? PATH_MAX : 1024] = "/proc/self/fd/3";
#ifdef F_GETPATH
  if (fcntl(3, F_GETPATH, path) == -1) snprintf(path, sizeof path, "/dev/fd/3");
#endif
  if (len > 12 && len - 12 < sizeof path) { memcpy(path, p + 12, len - 12); path[len - 12] = 0; }
  /* The JVM mounts the disc's JARs under java.io.tmpdir in a directory named
     after the disc, shared by every process: an ending session's clean-up
     removed a starting session's mount (BD-J start-up "io exception").
     Give each worker its own. */
  /* Java 18+ ignores a security manager unless the JVM allows one. libbluray
     installs its own anyway (a reflection workaround), but the JDK then skips
     its file checks, and those checks are what copy disc files into
     libbluray's cache on demand when the disc is an image: BD-J menus that
     read the disc's playlists or images themselves (Interstellar UHD) got
     "file not found" and drew nothing. */
  char opt[1200] = "-Djava.security.manager=allow";
  const char *store = getenv("BDNAVD_STORAGE");
  if (store && *store) {
    char d[1024];
    snprintf(d, sizeof d, "%s/tmp", store);
    mkdir(d, 0700);
    snprintf(own_tmp, sizeof own_tmp, "%s/tmp/%d", store, (int)getpid());
    if (mkdir(own_tmp, 0700) == 0 || errno == EEXIST)
      snprintf(opt, sizeof opt, "-Djava.security.manager=allow -Djava.io.tmpdir=%s", own_tmp);
  }
  setenv("JAVA_TOOL_OPTIONS", opt, 1);
  bd = bd_open(path, NULL);
  if (!bd) { reply_error("not_bluray"); return; }
  const BLURAY_DISC_INFO *di = bd_get_disc_info(bd);
  if (!di || !di->bluray_detected) { reply_error("not_bluray"); return; }
  if (di->aacs_detected && !di->aacs_handled) { reply_error("encrypted"); return; }
  if (di->bdplus_detected && !di->bdplus_handled) { reply_error("encrypted"); return; }
  char lang[4] = {0}, country[3] = {0};
  bd_set_player_setting(bd, BLURAY_PLAYER_SETTING_REGION_CODE, p[0]);
  memcpy(lang, p + 1, 3); bd_set_player_setting_str(bd, BLURAY_PLAYER_SETTING_AUDIO_LANG, lang);
  memcpy(lang, p + 4, 3); bd_set_player_setting_str(bd, BLURAY_PLAYER_SETTING_MENU_LANG, lang);
  memcpy(lang, p + 7, 3); bd_set_player_setting_str(bd, BLURAY_PLAYER_SETTING_PG_LANG, lang);
  memcpy(country, p + 10, 2); bd_set_player_setting_str(bd, BLURAY_PLAYER_SETTING_COUNTRY_CODE, country);
  bd_apply_player_caps(bd);
  bd_register_overlay_proc(bd, NULL, overlay);
  argb.lock = argb_lock_cb;
  argb.unlock = argb_unlock_cb;
  bd_register_argb_overlay_proc(bd, NULL, argb_overlay, &argb);
  /* BD-J storage, per session (the server's directory); without it BD-J keeps
     its defaults under the user's home. */
  const char *storage = getenv("BDNAVD_STORAGE");
  if (storage && *storage) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/persistent", storage);
    bd_set_player_setting_str(bd, BLURAY_PLAYER_PERSISTENT_ROOT, dir);
    /* The cache is per worker: libbluray clears it when a session starts and
       deletes from it as one ends, so a shared cache let an ending session
       delete files a starting one had just cached (BD-J start-up I/O errors). */
    snprintf(own_cache, sizeof own_cache, "%s/cache/%d", storage, (int)getpid());
    bd_set_player_setting_str(bd, BLURAY_PLAYER_CACHE_ROOT, own_cache);
  }
  /* Decode subtitles (PG); whether they show is the disc's (or the viewer's) choice. */
  bd_set_player_setting(bd, BLURAY_PLAYER_SETTING_DECODE_PG, 1);
  uint32_t titles = bd_get_titles(bd, TITLES_RELEVANT, 0);
  direct = di->num_bdj_titles > 0 && !di->bdj_handled;
  if (direct) {
    int main_title = bd_get_main_title(bd);
    if (main_title < 0 || !bd_select_title(bd, (uint32_t)main_title)) { reply_error("no_main_title"); return; }
    BLURAY_TITLE_INFO *ti = bd_get_title_info(bd, (uint32_t)main_title, 0);
    if (ti) { cur_playlist = ti->playlist; bd_free_title_info(ti); }
    /* Without the disc's menus nothing chooses subtitles: start with them hidden. */
    bd_select_stream(bd, BLURAY_PG_TEXTST_STREAM, 1, 0);
  } else if (!bd_play(bd)) {
    reply_error("play_failed");
    return;
  }
  j_begin();
  j("\"ok\":true,\"kind\":\"bluray\",\"volume\":\"%s\",\"titles\":%u,\"hdmv_titles\":%u,\"bdj_titles\":%u,"
    "\"first_play\":%d,\"top_menu\":%d,\"mode\":\"%s\",\"bdj_handled\":%d",
    di->udf_volume_id ? di->udf_volume_id : "", titles, di->num_hdmv_titles, di->num_bdj_titles, di->first_play_supported,
    di->top_menu_supported, direct ? "direct" : "menus", di->bdj_handled);
  j_end('R');
}

/* Menu clock. libbluray advances HDMV menu animation (page in/out effects, animated
   buttons) and the user time-out only inside bd_read_ext, against its own wall clock.
   The server stops reading while the client is buffered, so a graphics poll ticks
   libbluray with a zero-length read: no stream data, just the menu's clock and any
   navigation it runs. Events such a tick produces are held and given to the next
   read, so the server still sees every boundary where it cuts generations. */
#define HELD_MAX 64
static BD_EVENT held[HELD_MAX];
static int held_n;

static void menu_tick(void) {
  for (int i = 0; i < 16 && held_n < HELD_MAX; i++) {
    BD_EVENT ev;
    if (bd_read_ext(bd, readbuf, 0, &ev) < 0) break;
    if (ev.event == BD_EVENT_NONE) break;
    held[held_n++] = ev;
  }
}

static void cmd_next(const uint8_t *p, uint32_t len) {
  if (!bd) { reply_error("not_open"); return; }
  if (len < 4) { reply_error("bad_next"); return; }
  uint32_t max = p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
  if (max == 0 || max > MAX_UNITS) max = MAX_UNITS;
  bdj_watch();
  uint32_t limit = max * UNIT, delivered = 0;
  static int announced;
  if (direct && !announced) {
    /* No navigation events in direct mode: announce the playlist and its streams,
       after the events selecting the title queued (they set the audio stream). */
    announced = 1;
    drain_events();
    j_begin();
    j("\"ev\":\"playitem\",\"param\":0,");
    j_position();
    j_streams(0);
    j_end('E');
  }
  const char *stop = "count";
  /* events from menu ticks first, in order; the first stop among them ends this read */
  const char *held_stop = NULL;
  for (int i = 0; i < held_n; i++) {
    const char *t = event(&held[i]);
    if (!held_stop) held_stop = t;
  }
  held_n = 0;
  if (held_stop) stop = held_stop;
  while (!held_stop && delivered < limit) {
    BD_EVENT ev;
    /* Read into a separate buffer: overlays fire inside the read and flush `batch`. */
    int r = bd_read_ext(bd, readbuf, (int)(limit - delivered), &ev);
    if (r < 0) { stop = "read_failed"; break; }
    if (r > 0) {
      memcpy(batch + batch_len, readbuf, (size_t)r);
      batch_len += (uint32_t)r;
      delivered += (uint32_t)r;
      bdj_bytes += (uint64_t)r;
    }
    const char *s = event(&ev);
    BD_EVENT more;
    while (bd_get_event(bd, &more)) {
      const char *t = event(&more);
      if (!s) s = t;
    }
    if (s) { stop = s; break; }
    if (r == 0) { stop = "empty"; break; }
  }
  flush_batch();
  j_begin();
  j("\"ok\":true,\"stop\":\"%s\",\"bytes\":%u,", stop, delivered);
  j_position();
  j_end('R');
}

static int64_t get_i64(const uint8_t *p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
  return (int64_t)v;
}

static void cmd_key(const uint8_t *p, uint32_t len) {
  if (!bd) { reply_error("not_open"); return; }
  if (len < 12) { reply_error("bad_key"); return; }
  uint32_t key = p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
  int r = bd_user_input(bd, get_i64(p + 4), key);
  drain_events();
  j_begin();
  j("\"ok\":%s,\"result\":%d,", r < 0 ? "false" : "true", r);
  if (r < 0) j("\"error\":\"refused\",");
  j_position();
  j_end('R');
}

static void cmd_menu(const uint8_t *p, uint32_t len) {
  if (!bd) { reply_error("not_open"); return; }
  int r = bd_menu_call(bd, len >= 8 ? get_i64(p) : -1);
  drain_events();
  j_begin();
  j("\"ok\":%s,", r ? "true" : "false");
  if (!r) j("\"error\":\"refused\",");
  j_position();
  j_end('R');
}

static void cmd_chapter(const uint8_t *p, uint32_t len) {
  if (!bd) { reply_error("not_open"); return; }
  if (len < 4) { reply_error("bad_chapter"); return; }
  uint32_t ch = p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
  int64_t r = bd_seek_chapter(bd, ch);
  drain_events();
  j_begin();
  j("\"ok\":%s,\"pos\":%" PRId64 ",", r < 0 ? "false" : "true", r);
  if (r < 0) j("\"error\":\"refused\",");
  j_position();
  j_end('R');
}

/* 'A' audio u32 stream (1-based); 'S' subtitles u32 stream (1-based), u8 enable. */
static void cmd_stream(char c, const uint8_t *p, uint32_t len) {
  if (!bd) { reply_error("not_open"); return; }
  if (len < (c == 'S' ? 5u : 4u)) { reply_error("bad_stream"); return; }
  uint32_t n = p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
  int ok = bd_select_stream(bd, c == 'A' ? BLURAY_AUDIO_STREAM : BLURAY_PG_TEXTST_STREAM, n,
                            c == 'A' ? 1 : p[4]);
  if (ok) {
    if (c == 'A') cur_audio = n;
    else { cur_pg = n; pg_on = p[4]; }
  }
  drain_events();
  j_begin();
  j("\"ok\":%s,", ok ? "true" : "false");
  if (!ok) j("\"error\":\"refused\",");
  j_position();
  j_end('R');
}

/* 'T' seek u64 tick: 90 kHz time in the current playlist. */
static void cmd_seek(const uint8_t *p, uint32_t len) {
  if (!bd) { reply_error("not_open"); return; }
  if (len < 8) { reply_error("bad_seek"); return; }
  int64_t r = bd_seek_time(bd, (uint64_t)get_i64(p));
  drain_events();
  j_begin();
  j("\"ok\":%s,\"pos\":%" PRId64 ",", r < 0 ? "false" : "true", r);
  if (r < 0) j("\"error\":\"refused\",");
  j_position();
  j_end('R');
}

int main(void) {
  static uint8_t payload[1100];
  proto_out = dup(STDOUT_FILENO);
  if (proto_out < 0 || dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
    fprintf(stderr, "bdnavd: cannot set up output\n");
    return 2;
  }
  /* stdout and stderr (ours, libbluray's, the JVM's) go through log_relay. */
  int logp[2];
  pthread_t relay;
  real_stderr = dup(STDERR_FILENO);
  if (real_stderr >= 0 && pipe(logp) == 0 && dup2(logp[1], STDERR_FILENO) >= 0 &&
      dup2(logp[1], STDOUT_FILENO) >= 0 &&
      pthread_create(&relay, NULL, log_relay, (void *)(intptr_t)logp[0]) == 0) {
    close(logp[1]);
    pthread_detach(relay);
    setvbuf(stderr, NULL, _IONBF, 0);
  } else {
    fprintf(stderr, "bdnavd: log relay unavailable; start failures are found by the drawing watchdog only\n");
  }
  send_hello();
  for (;;) {
    uint8_t head[5];
    if (read_all(head, 5) < 0) break;
    uint32_t total = head[0] | (head[1] << 8) | (head[2] << 16) | ((uint32_t)head[3] << 24);
    if (total < 1 || total - 1 > sizeof payload) {
      fprintf(stderr, "bdnavd: bad frame length %u\n", total);
      return 2;
    }
    uint32_t len = total - 1;
    if (len > 0 && read_all(payload, len) < 0) break;
    switch ((char)head[4]) {
      case 'O': cmd_open(payload, len); break;
      case 'N': cmd_next(payload, len); break;
      case 'K': cmd_key(payload, len); break;
      case 'M': cmd_menu(payload, len); break;
      case 'C': cmd_chapter(payload, len); break;
      case 'A': case 'S': cmd_stream((char)head[4], payload, len); break;
      case 'T': cmd_seek(payload, len); break;
      case 'G': /* poll: BD-J graphics drawn since the last reply (sent before every reply) */
        if (!bd) { reply_error("not_open"); break; }
        bdj_watch();
        menu_tick();
        j_begin(); j("\"ok\":true,"); j_position(); j_end('R');
        break;
      case 'X': j_begin(); j("\"ok\":true"); j_end('R'); goto quit;
      default: reply_error("bad_command"); break;
    }
  }
quit:
  if (bd) bd_close(bd);
  remove_tree(own_cache);
  remove_tree(own_tmp);
  for (int i = 0; i < 2; i++) free(plane[i]);
  return 0;
}
