/*
 * navd: DVD-Video navigation worker.
 *
 * Runs libdvdnav's DVD virtual machine over a decrypted DVD-Video image for
 * one viewing session, under a supervising server. This process never
 * opens the disc itself: it reads it through the server's read service
 * (readsvc.h, fds 3 and 4). An image is read through libdvdread's stream
 * callbacks; a disc folder (VIDEO_TS inside) through libdvdread's file-system
 * callbacks (DVDOpenFiles, passed through by patches/libdvdnav).
 *
 * Licence: links libdvdnav and libdvdread (GPL-2.0-or-later), so this program
 * is GPL-2.0-or-later.
 *
 * Protocol, stdin/stdout, frames: u32 little-endian length of (type + payload),
 * u8 type, payload. Each command produces zero or more 'D'/'E' frames and then
 * exactly one 'R' frame.
 *
 *   Commands (server -> navd)
 *     'O' open    u32 region_mask, u8 parental_level, 6 bytes menu/audio/spu language,
 *                 u16 SPRM 14 (the display: aspect in bits 10-11, mode in bits 8-9)
 *     'N' next    u32 max_blocks: read until max_blocks, a still, wait, hop, title-set change or stop
 *     'B' button  u8 op (1 up 2 down 3 left 4 right 5 activate 6 select), u8 button, u32 pci_id
 *     'M' menu    u8 DVDMenuID_t
 *     'S' still_skip   'W' wait_skip   'U' go_up   'Q' state   'X' quit
 *     'T' play    u16 title, u16 part (0 plays the title from its start)
 *     'R' resync  u32 pci_id: move the VM to that PCI's VOBU (the picture on screen),
 *                 so a following menu call saves the displayed position for resume
 *     'L' subtitles u8 op: 0 hide, 1 the next stream the title has (after the last: hide),
 *                 2 show logical stream u8 n (protocol 7; "no_such_stream" when the
 *                 program chain lacks it)
 *     'J' seek    u64 90 kHz time in the current program chain
 *     'V' angle   u8 angle (1-based; 0 the next one)
 *   On entering a title's program chain (protocol 7), before its first "cell" event:
 *     {"ev":"title_info","title":t,"pgc":n,"length_90k":L,"chapters_90k":[start, ...],
 *      "audio":[{"logical":i,"lang":"en","format":f,"channels":c,"ext":e}, ...],
 *      "spu":[{"logical":i,"lang":"en","ext":e}, ...]}
 *   Times are in the program chain (the time 'J' seeks to); a chapter is a program.
 *   Positions (replies, "vts" and "cell" events) add "spst" (protocol 7): the logical
 *   subpicture stream SPRM 2 names, shown when "spu_on" is 1.
 *   "format" is libdvdnav's DVD_AUDIO_FORMAT_* (0 AC-3, 3 MPEG, 4 LPCM, 6 DTS,
 *   7 SDDS, -1 unknown); "ext" the stream's code extension (audio: 1 normal,
 *   2 visually impaired, 3 and 4 director's comments; subpictures: 1 normal,
 *   2 large, 3 children's, 5 to 7 captions, 9 forced, 13 to 15 director's
 *   comments); "lang" is empty when the disc names none.
 *   Commands the disc forbids here (its user-operation masks) are refused with
 *   "uop_prohibited". A read that reaches CSS-scrambled sectors (an encrypted
 *   disc) is refused with "encrypted".
 *   Replies (navd -> server)
 *     'D' data    n * 2048-byte sectors, in delivery order
 *     'E' event   one JSON object
 *     'R' reply   one JSON object: {"ok":true,...} or {"ok":false,"error":"..."}
 */
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <dvdnav/dvdnav.h>
#include <dvdread/dvd_reader.h>
#include <dvdread/nav_types.h>
/* Internal headers of the pinned libdvdnav source, for register diagnostics only,
 * in the order libdvdnav's own sources include them. */
#include "config.h"
#include "vm/decoder.h"
#include "vm/vm.h"
#include "dvdnav_internal.h"
#include "readsvc.h"

#define SECTOR 2048
#define MAX_BATCH 512          /* sectors per 'D' frame and per 'N' command */
#define PCI_RING 64
#define JSON_MAX 16384
/* The disc folder's root as libdvdread sees it; every path under it goes to
   the read service relative to the disc root. */
#define FOLDER_ROOT "/disc"

static dvdnav_t *nav;

/* The last error or warning libdvdnav/libdvdread logged, JSON-safe: why a disc stopped
   (e.g. "Error opening vtsN=1") reaches the server instead of a bare stop. */
static char last_error[256];


/* The viewer's audio language (ISO 639-1, e.g. "en"); 0 = none. */
static uint16_t pref_audio;

/* libdvdnav stores the audio language preference (SPRM 16) for the disc's own commands but
   never chooses a stream by it: with SPRM 1 unset (15) a title plays its first stream. A player
   chooses: the first stream in the viewer's language, a director's commentary only if nothing
   else matches. Returns the logical stream chosen, or -1 (none matches: the disc's default). */
static int choose_audio_by_language(void) {
  if (!pref_audio || !dvdnav_is_domain_vts(nav)) return -1;
  int fallback = -1;
  for (int i = 0; i < 8; i++) {
    audio_attr_t a;
    if (dvdnav_get_audio_logical_stream(nav, (uint8_t)i) < 0) continue;
    if (dvdnav_get_audio_attr(nav, (uint8_t)i, &a) != DVDNAV_STATUS_OK) continue;
    if (a.lang_code != pref_audio) continue;
    if (a.code_extension == 3 || a.code_extension == 4) { if (fallback < 0) fallback = i; continue; }
    return i;
  }
  return fallback;
}

static uint64_t image_size;
static uint64_t image_pos;

static pci_t pci_ring[PCI_RING];
static uint32_t pci_ids[PCI_RING];
static uint32_t next_pci_id = 1;

static uint8_t batch[MAX_BATCH * SECTOR];
static uint32_t batch_n;

/* ---------- framing ---------- */

static int write_all(const void *p, size_t n) {
  const uint8_t *b = p;
  while (n > 0) {
    ssize_t w = write(STDOUT_FILENO, b, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    b += w;
    n -= (size_t)w;
  }
  return 0;
}

static void frame(char type, const void *payload, uint32_t len) {
  uint8_t head[5];
  uint32_t total = len + 1;
  head[0] = total & 0xff;
  head[1] = (total >> 8) & 0xff;
  head[2] = (total >> 16) & 0xff;
  head[3] = (total >> 24) & 0xff;
  head[4] = (uint8_t)type;
  if (write_all(head, 5) < 0 || (len > 0 && write_all(payload, len) < 0)) {
    /* The supervisor is gone; nothing useful remains to do. */
    _exit(3);
  }
}

static int read_all(void *p, size_t n) {
  uint8_t *b = p;
  while (n > 0) {
    ssize_t r = read(STDIN_FILENO, b, n);
    if (r == 0) return -1; /* supervisor closed stdin */
    if (r < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    b += r;
    n -= (size_t)r;
  }
  return 0;
}

/* ---------- JSON ---------- */

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

static void j_end(char type) {
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
  if (nav) j(",\"detail\":\"%s%s%s\"", dvdnav_err_to_string(nav), last_error[0] ? "; " : "", last_error);
  j_end('R');
}

/* ---------- image stream ---------- */

static int image_seek(void *priv, uint64_t pos) {
  (void)priv;
  if (pos > image_size) return -1;
  image_pos = pos;
  return 0;
}

static int image_read(void *priv, void *buffer, int len) {
  (void)priv;
  if (len <= 0) return 0;
  int64_t r = rs_read(image_pos, buffer, (size_t)len);
  if (r < 0) {
    snprintf(last_error, sizeof last_error, "read_failed at %" PRIu64, image_pos);
    return -1;
  }
  image_pos += (uint64_t)r;
  return (int)r;
}

static dvdnav_stream_cb stream_cb = {image_seek, image_read, NULL};

/* ---------- folder file system (read service) ---------- */

static int fs_rel(const char *path, char *rel) {
  return rs_relative(FOLDER_ROOT, path, rel, RS_MAX_PATH + 1);
}

static void fs_close_cb(dvd_reader_filesystem_h *fs) { (void)fs; }

static int fs_stat(dvd_reader_filesystem_h *fs, const char *path, dvdstat_t *st) {
  (void)fs;
  char rel[RS_MAX_PATH + 1];
  int is_dir = 0;
  uint64_t size = 0;
  if (fs_rel(path, rel) < 0 || rs_stat(rel, &is_dir, &size) != RS_OK) return -1;
  st->size = size;
  st->st_mode = is_dir ? DVD_S_IFDIR : DVD_S_IFREG;
  return 0;
}

static void *fs_dir_open(dvd_reader_filesystem_h *fs, const char *dirname) {
  (void)fs;
  char rel[RS_MAX_PATH + 1];
  rs_list *list = calloc(1, sizeof *list);
  if (!list || fs_rel(dirname, rel) < 0 || rs_list_open(rel, list) != RS_OK) {
    free(list);
    return NULL;
  }
  return list;
}

static int fs_dir_read(void *dir, dvd_dirent_t *entry) {
  return rs_list_next(dir, entry->d_name, sizeof entry->d_name);
}

static void fs_dir_close(void *dir) {
  if (!dir) return;
  rs_list_free(dir);
  free(dir);
}

static void *fs_file_open(dvd_reader_filesystem_h *fs, const char *filename) {
  (void)fs;
  char rel[RS_MAX_PATH + 1];
  rs_file *file = calloc(1, sizeof *file);
  if (!file || fs_rel(filename, rel) < 0 || rs_open(rel, file) != RS_OK) {
    free(file);
    return NULL;
  }
  return file;
}

static ssize_t fs_file_read(void *file, char *buf, size_t size) {
  int64_t r = rs_fread(file, buf, size);
  if (r < 0) snprintf(last_error, sizeof last_error, "read_failed in a disc file");
  return (ssize_t)r;
}

static off64_t fs_file_seek(void *file, off64_t offset, int whence) {
  rs_file *f = file;
  if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) return -1;
  int64_t base = whence == SEEK_CUR ? (int64_t)f->pos : whence == SEEK_END ? (int64_t)f->size : 0;
  int64_t to = base + (int64_t)offset;
  if (to < 0) return -1;
  f->pos = (uint64_t)to;
  return (off64_t)to;
}

static int fs_file_close(void *file) {
  if (!file) return 0;
  rs_close(file);
  free(file);
  return 0;
}

static dvd_reader_filesystem_h folder_fs = {
    NULL, fs_close_cb, fs_stat, fs_dir_open, fs_dir_read, fs_dir_close,
    fs_file_open, fs_file_read, fs_file_seek, fs_file_close,
};

static void log_cb(void *priv, dvdnav_logger_level_t level, const char *fmt, va_list args) {
  (void)priv;
  va_list copy;
  va_copy(copy, args);
  fprintf(stderr, "navd[%d] ", (int)level);
  vfprintf(stderr, fmt, args);
  fputc('\n', stderr);
  if (level == DVDNAV_LOGGER_LEVEL_ERROR || level == DVDNAV_LOGGER_LEVEL_WARN) {
    char raw[256];
    vsnprintf(raw, sizeof raw, fmt, copy);
    size_t n = 0;
    for (const char *c = raw; *c && n + 1 < sizeof last_error; c++)
      if (*c != '"' && *c != '\\' && (unsigned char)*c >= 0x20) last_error[n++] = *c;
    last_error[n] = 0;
  }
  va_end(copy);
}

static dvdnav_logger_cb logger = {log_cb};

/* ---------- helpers ---------- */

static void flush_batch(void) {
  if (batch_n > 0) {
    frame('D', batch, batch_n * SECTOR);
    batch_n = 0;
  }
}

static const char *domain(void) {
  if (dvdnav_is_domain_fp(nav)) return "fp";
  if (dvdnav_is_domain_vmgm(nav)) return "vmgm";
  if (dvdnav_is_domain_vtsm(nav)) return "vtsm";
  if (dvdnav_is_domain_vts(nav)) return "vts";
  return "unknown";
}

static void j_position(void) {
  int32_t title = 0, part = 0, button = 0;
  dvdnav_current_title_info(nav, &title, &part);
  dvdnav_get_current_highlight(nav, &button);
  uint32_t w = 0, h = 0;
  dvdnav_get_video_resolution(nav, &w, &h);
  int32_t angle = 1, angles = 1;
  dvdnav_get_angle_info(nav, &angle, &angles);
  j("\"domain\":\"%s\",\"title\":%d,\"part\":%d,\"button\":%d,", domain(), title, part, button);
  j("\"angle\":%d,\"angles\":%d,\"spu_on\":%d,\"spst\":%d,", angle, angles,
    (nav->vm->state.registers.SPRM[2] & 0x40) ? 1 : 0, nav->vm->state.registers.SPRM[2] & 0x3f);
  j("\"audio\":%d,\"spu\":%d,\"aspect\":%u,\"scale\":%u,\"width\":%u,\"height\":%u",
    dvdnav_get_active_audio_stream(nav), dvdnav_get_active_spu_stream(nav),
    dvdnav_get_video_aspect(nav), dvdnav_get_video_scale_permission(nav), w, h);
}

static void j_registers(void) {
  registers_t *r = &nav->vm->state.registers;
  j(",\"sprm\":[");
  for (int i = 0; i < 24; i++) j("%s%u", i ? "," : "", r->SPRM[i]);
  j("],\"gprm\":[");
  for (int i = 0; i < 16; i++) j("%s%u", i ? "," : "", r->GPRM[i]);
  j("]");
}

static uint32_t store_pci(const pci_t *pci) {
  uint32_t id = next_pci_id++;
  uint32_t slot = id % PCI_RING;
  pci_ring[slot] = *pci;
  pci_ids[slot] = id;
  return id;
}

static pci_t *find_pci(uint32_t id) {
  uint32_t slot = id % PCI_RING;
  return (id != 0 && pci_ids[slot] == id) ? &pci_ring[slot] : NULL;
}

static void event_nav(const pci_t *pci, const dsi_t *dsi, uint32_t id) {
  const hl_gi_t *g = &pci->hli.hl_gi;
  j_begin();
  j("\"ev\":\"nav\",\"pci_id\":%u,\"lbn\":%u,\"vobu_s_ptm\":%u,\"vobu_e_ptm\":%u,\"vobu_se_e_ptm\":%u,",
    id, pci->pci_gi.nv_pck_lbn, pci->pci_gi.vobu_s_ptm, pci->pci_gi.vobu_e_ptm,
    pci->pci_gi.vobu_se_e_ptm);
  j("\"vobu_c_idn\":%u,\"c_eltm_s\":%u,", dsi->dsi_gi.vobu_c_idn,
    (unsigned)dsi->dsi_gi.c_eltm.second);
  j("\"hli_ss\":%u,\"hli_s_ptm\":%u,\"hli_e_ptm\":%u,\"btn_se_e_ptm\":%u,", g->hli_ss & 3,
    g->hli_s_ptm, g->hli_e_ptm, g->btn_se_e_ptm);
  j("\"btngr_ns\":%u,\"btngr_dsp_ty\":[%u,%u,%u],\"btn_ofn\":%u,\"btn_ns\":%u,\"nsl_btn_ns\":%u,",
    g->btngr_ns, g->btngr1_dsp_ty, g->btngr2_dsp_ty, g->btngr3_dsp_ty, g->btn_ofn,
    g->btn_ns & 0x3f, g->nsl_btn_ns & 0x3f);
  j("\"fosl_btnn\":%u,\"foac_btnn\":%u,\"btn_coli\":[", g->fosl_btnn & 0x3f, g->foac_btnn & 0x3f);
  for (int c = 0; c < 3; c++)
    j("%s[%u,%u]", c ? "," : "", pci->hli.btn_colit.btn_coli[c][0], pci->hli.btn_colit.btn_coli[c][1]);
  j("],\"buttons\":[");
  int count = 0;
  if ((g->hli_ss & 3) != 0) {
    int n = g->btn_ns & 0x3f;
    if (n > 36) n = 36;
    for (int i = 0; i < n; i++) {
      const btni_t *b = &pci->hli.btnit[i];
      j("%s{\"n\":%d,\"x0\":%u,\"x1\":%u,\"y0\":%u,\"y1\":%u,\"up\":%u,\"down\":%u,\"left\":%u,"
        "\"right\":%u,\"auto\":%u,\"coln\":%u,\"cmd\":\"",
        count++ ? "," : "", i + 1, b->x_start, b->x_end, b->y_start, b->y_end, b->up, b->down,
        b->left, b->right, b->auto_action_mode, b->btn_coln);
      for (int k = 0; k < 8; k++) j("%02x", b->cmd.bytes[k]);
      j("\"}");
    }
  }
  j("]");
  j_end('E');
}

/* A sector still CSS-scrambled: its PES header's scrambling control bits are
   set (descrambling clears them). Decrypted discs play; this one cannot. */
static int scrambled(const uint8_t *s) {
  if (s[0] != 0 || s[1] != 0 || s[2] != 1 || s[3] != 0xBA) return 0;
  size_t pes = 14 + (s[13] & 7);
  if (pes + 7 > SECTOR) return 0;
  const uint8_t *p = s + pes;
  int id = p[3];
  int media = id == 0xE0 || id == 0xBD || (id & 0xE0) == 0xC0;
  return p[0] == 0 && p[1] == 0 && p[2] == 1 && media && (p[6] & 0xC0) == 0x80 && (p[6] & 0x30) != 0;
}

/* ---------- title information (protocol 7) ---------- */

/* The program chain last described: a new one is described once. */
static const pgc_t *described_pgc;
static int32_t described_title = -1;

static void j_lang(uint16_t code) {
  char a = (char)(code >> 8), b = (char)(code & 0xff);
  if (a >= 'a' && a <= 'z' && b >= 'a' && b <= 'z') j("\"lang\":\"%c%c\"", a, b);
  else if (a >= 'A' && a <= 'Z' && b >= 'A' && b <= 'Z') j("\"lang\":\"%c%c\"", a + 32, b + 32);
  else j("\"lang\":\"\"");
}

/* A cell's playback time, unless it is a later angle of an angle block
   (those play instead of the first, not after it). */
static int64_t cell_ticks(const pgc_t *pgc, int cell) {
  const cell_playback_t *c = &pgc->cell_playback[cell];
  if (c->block_type == BLOCK_TYPE_ANGLE_BLOCK && c->block_mode != BLOCK_MODE_FIRST_CELL) return 0;
  dvd_time_t t = c->playback_time;
  return dvdnav_convert_time(&t);
}

static void event_title_info(int32_t title) {
  const pgc_t *pgc = nav->vm->state.pgc;
  if (!pgc || (pgc == described_pgc && title == described_title)) return;
  described_pgc = pgc;
  described_title = title;
  dvd_time_t total = pgc->playback_time;
  j_begin();
  j("\"ev\":\"title_info\",\"title\":%d,\"pgc\":%d,\"length_90k\":%" PRId64 ",\"chapters_90k\":[", title,
    nav->vm->state.pgcN, dvdnav_convert_time(&total));
  /* Each program's start: the playback time of the cells before its entry cell. */
  int64_t at = 0;
  int cell = 0;
  for (int p = 0; p < pgc->nr_of_programs && pgc->program_map; p++) {
    int entry = pgc->program_map[p] - 1;
    while (cell < entry && cell < pgc->nr_of_cells && pgc->cell_playback) at += cell_ticks(pgc, cell++);
    j("%s%" PRId64, p ? "," : "", at);
  }
  j("],\"audio\":[");
  int n = 0;
  for (int i = 0; i < 8; i++) {
    if (dvdnav_get_audio_logical_stream(nav, (uint8_t)i) < 0) continue;
    audio_attr_t a;
    memset(&a, 0, sizeof a);
    dvdnav_get_audio_attr(nav, (uint8_t)i, &a);
    int format = (int)dvdnav_audio_stream_format(nav, (uint8_t)i);
    if (format == 0xffff) format = -1;
    int channels = (int)dvdnav_audio_stream_channels(nav, (uint8_t)i);
    if (channels == 0xffff) channels = 0;
    j("%s{\"logical\":%d,", n++ ? "," : "", i);
    j_lang(dvdnav_audio_stream_to_lang(nav, (uint8_t)i));
    j(",\"format\":%d,\"channels\":%d,\"ext\":%u}", format, channels, (unsigned)a.code_extension);
  }
  j("],\"spu\":[");
  n = 0;
  for (int i = 0; i < 32; i++) {
    if (dvdnav_get_spu_logical_stream(nav, (uint8_t)i) < 0) continue;
    subp_attr_t s;
    memset(&s, 0, sizeof s);
    dvdnav_get_spu_attr(nav, (uint8_t)i, &s);
    j("%s{\"logical\":%d,", n++ ? "," : "", i);
    j_lang(dvdnav_spu_stream_to_lang(nav, (uint8_t)i));
    j(",\"ext\":%u}", (unsigned)s.code_extension);
  }
  j("]");
  j_end('E');
}

/* ---------- commands ---------- */

static void cmd_open(const uint8_t *p, uint32_t len) {
  if (nav) {
    reply_error("already_open");
    return;
  }
  if (len < 4 + 1 + 6 + 2) {
    reply_error("bad_open");
    return;
  }
  int layout = 0;
  if (rs_info(&layout, &image_size) != 0) {
    reply_error("no_disc");
    return;
  }
  if (layout == RS_FOLDER) {
    if (dvdnav_open_files(&nav, NULL, &logger, FOLDER_ROOT, &folder_fs) != DVDNAV_STATUS_OK || !nav) {
      nav = NULL;
      reply_error("open_failed");
      return;
    }
  } else {
    /* libdvdread takes the stream path only with a non-NULL private pointer (dvd_reader.c). */
    if (dvdnav_open_stream2(&nav, &image_pos, &logger, &stream_cb) != DVDNAV_STATUS_OK || !nav) {
      nav = NULL;
      reply_error("open_failed");
      return;
    }
  }
  int32_t region = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
  char menu[3] = {(char)p[5], (char)p[6], 0}, audio[3] = {(char)p[7], (char)p[8], 0},
       spu[3] = {(char)p[9], (char)p[10], 0};
  dvdnav_set_readahead_flag(nav, 0);
  dvdnav_set_PGC_positioning_flag(nav, 1);
  dvdnav_set_region_mask(nav, region);
  dvdnav_menu_language_select(nav, menu);
  dvdnav_audio_language_select(nav, audio);
  pref_audio = (uint16_t)(((uint8_t)audio[0] << 8) | (uint8_t)audio[1]);
  dvdnav_spu_language_select(nav, spu);
  /* SPRM 13 parental level; SPRM 14 the viewer's display, from the session. */
  nav->vm->state.registers.SPRM[13] = p[4];
  nav->vm->state.registers.SPRM[14] = (uint16_t)(p[11] | (p[12] << 8)) & 0x0f00;
  /* libdvdnav does not enforce the disc's regions: report them for the server. */
  int32_t disc_regions = 0xff;
  if (dvdnav_get_disk_region_mask(nav, &disc_regions) != DVDNAV_STATUS_OK) disc_regions = 0xff;
  int32_t titles = 0;
  dvdnav_get_number_of_titles(nav, &titles);
  const char *volid = NULL;
  dvdnav_get_title_string(nav, &volid);
  j_begin();
  j("\"ok\":true,\"titles\":%d,\"image_bytes\":%" PRIu64 ",\"disc_regions\":%d,\"volume\":\"", titles,
    image_size, disc_regions & 0xff);
  for (const char *c = volid ? volid : ""; *c; c++)
    if (*c >= 0x20 && *c < 0x7f && *c != '"' && *c != '\\') j("%c", *c);
  j("\"");
  j_end('R');
}

static void cmd_next(const uint8_t *p, uint32_t len) {
  if (!nav || len < 4) {
    reply_error(nav ? "bad_next" : "not_open");
    return;
  }
  uint32_t max = p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
  if (max == 0 || max > MAX_BATCH) max = MAX_BATCH;
  uint8_t block[SECTOR];
  uint32_t delivered = 0;
  const char *stop = "budget";
  int32_t still_length = -1;
  while (delivered < max) {
    int32_t event = 0, elen = 0;
    if (dvdnav_get_next_block(nav, block, &event, &elen) != DVDNAV_STATUS_OK) {
      flush_batch();
      reply_error("read_failed");
      return;
    }
    switch (event) {
      case DVDNAV_BLOCK_OK:
        if (scrambled(block)) {
          /* Nothing scrambled is delivered. */
          batch_n = 0;
          reply_error("encrypted");
          return;
        }
        memcpy(batch + batch_n * SECTOR, block, SECTOR);
        batch_n++;
        delivered++;
        break;
      case DVDNAV_NAV_PACKET: {
        memcpy(batch + batch_n * SECTOR, block, SECTOR);
        batch_n++;
        delivered++;
        flush_batch();
        pci_t *pci = dvdnav_get_current_nav_pci(nav);
        dsi_t *dsi = dvdnav_get_current_nav_dsi(nav);
        if (pci && dsi) event_nav(pci, dsi, store_pci(pci));
        break;
      }
      case DVDNAV_NOP:
        break;
      case DVDNAV_STILL_FRAME: {
        dvdnav_still_event_t *e = (dvdnav_still_event_t *)block;
        still_length = e->length;
        flush_batch();
        j_begin();
        j("\"ev\":\"still\",\"length\":%d", e->length);
        j_end('E');
        stop = "still";
        goto done;
      }
      case DVDNAV_WAIT:
        flush_batch();
        j_begin();
        j("\"ev\":\"wait\"");
        j_end('E');
        stop = "wait";
        goto done;
      case DVDNAV_SPU_CLUT_CHANGE: {
        const uint32_t *clut = (const uint32_t *)block;
        flush_batch();
        j_begin();
        j("\"ev\":\"clut\",\"clut\":[");
        for (int i = 0; i < 16; i++) j("%s%u", i ? "," : "", clut[i]);
        j("]");
        j_end('E');
        break;
      }
      case DVDNAV_SPU_STREAM_CHANGE: {
        dvdnav_spu_stream_change_event_t *e = (dvdnav_spu_stream_change_event_t *)block;
        flush_batch();
        j_begin();
        j("\"ev\":\"spu_stream\",\"wide\":%d,\"letterbox\":%d,\"pan_scan\":%d,\"logical\":%d",
          e->physical_wide, e->physical_letterbox, e->physical_pan_scan, e->logical);
        j_end('E');
        break;
      }
      case DVDNAV_AUDIO_STREAM_CHANGE: {
        dvdnav_audio_stream_change_event_t *e = (dvdnav_audio_stream_change_event_t *)block;
        flush_batch();
        j_begin();
        /* The physical number counts within each codec (AC-3 0x80+n, MPEG 0xC0+n, LPCM 0xA0+n,
           DTS 0x88+n): report the codec too, or "physical 0" names several streams. */
        /* The logical stream the VM plays (libdvdnav vm_get_audio_active_stream): stream 0
           outside titles; in a title SPRM 1, or the first stream that exists when SPRM 1 names
           none (15, "not chosen", is common). */
        int lg = e->logical;
        int physical = e->physical;
        /* A title entered with no stream chosen (SPRM 1 unset or not a stream here): choose by
           the viewer's language, as a player does. The VM switches at its next VOBU. */
        if (dvdnav_is_domain_vts(nav) && (lg < 0 || lg > 7 || dvdnav_get_audio_logical_stream(nav, (uint8_t)lg) < 0)) {
          int want = choose_audio_by_language();
          if (want >= 0 && dvdnav_set_active_stream(nav, (uint8_t)want, DVD_AUDIO_STREAM) == DVDNAV_STATUS_OK) {
            lg = want;
            physical = dvdnav_get_audio_logical_stream(nav, (uint8_t)want);
            fprintf(stderr, "navd: audio by language: logical %d (physical %d)\n", lg, physical);
          }
        }
        if (physical < 0) lg = -1;
        else if (!dvdnav_is_domain_vts(nav)) lg = 0;
        else if (lg < 0 || lg > 7 || dvdnav_get_audio_logical_stream(nav, (uint8_t)lg) < 0) {
          lg = -1;
          for (int i = 0; i < 8; i++)
            if (dvdnav_get_audio_logical_stream(nav, (uint8_t)i) >= 0) { lg = i; break; }
        }
        int format = lg >= 0 ? (int)dvdnav_audio_stream_format(nav, (uint8_t)lg) : -1;
        if (format == 0xffff) format = -1;
        j("\"ev\":\"audio_stream\",\"physical\":%d,\"logical\":%d,\"format\":%d,\"sprm1\":%d",
          physical, lg, format, e->logical);
        j_end('E');
        break;
      }
      case DVDNAV_VTS_CHANGE: {
        dvdnav_vts_change_event_t *e = (dvdnav_vts_change_event_t *)block;
        flush_batch();
        j_begin();
        j("\"ev\":\"vts\",\"old_vts\":%d,\"new_vts\":%d,\"old_domain\":%d,\"new_domain\":%d,",
          e->old_vtsN, e->new_vtsN, (int)e->old_domain, (int)e->new_domain);
        j_position();
        j_end('E');
        /* A domain or title-set change always ends a response, so the server can
           cut a generation exactly there. */
        stop = "vts";
        goto done;
      }
      case DVDNAV_CELL_CHANGE: {
        dvdnav_cell_change_event_t *e = (dvdnav_cell_change_event_t *)block;
        flush_batch();
        if (dvdnav_is_domain_vts(nav)) {
          int32_t t = 0, part = 0;
          dvdnav_current_title_info(nav, &t, &part);
          event_title_info(t);
        } else {
          described_pgc = NULL;
          described_title = -1;
        }
        j_begin();
        j("\"ev\":\"cell\",\"cell\":%d,\"pg\":%d,\"cell_sectors\":%" PRId64 ",\"pgc_ticks\":%" PRId64
          ",\"cell_start\":%" PRId64 ",",
          e->cellN, e->pgN, e->cell_length, e->pgc_length, e->cell_start);
        j_position();
        j_end('E');
        break;
      }
      case DVDNAV_HIGHLIGHT: {
        dvdnav_highlight_event_t *e = (dvdnav_highlight_event_t *)block;
        flush_batch();
        j_begin();
        j("\"ev\":\"highlight\",\"display\":%d,\"button\":%u", e->display, e->buttonN);
        j_end('E');
        break;
      }
      case DVDNAV_HOP_CHANNEL:
        flush_batch();
        j_begin();
        j("\"ev\":\"hop\"");
        j_end('E');
        stop = "hop";
        goto done;
      case DVDNAV_STOP:
        flush_batch();
        j_begin();
        j("\"ev\":\"stop\"");
        j_end('E');
        stop = "stop";
        goto done;
      default:
        flush_batch();
        j_begin();
        j("\"ev\":\"unknown\",\"code\":%d", event);
        j_end('E');
        break;
    }
    if (batch_n == MAX_BATCH) flush_batch();
  }
done:
  flush_batch();
  j_begin();
  j("\"ok\":true,\"stop\":\"%s\",\"blocks\":%u", stop, delivered);
  if (still_length >= 0) j(",\"still_length\":%d", still_length);
  if (!strcmp(stop, "stop") && last_error[0]) j(",\"detail\":\"%s\"", last_error);
  j(",");
  j_position();
  j_end('R');
}

static void cmd_button(const uint8_t *p, uint32_t len) {
  if (!nav || len < 6) {
    reply_error(nav ? "bad_button" : "not_open");
    return;
  }
  uint8_t op = p[0], button = p[1];
  uint32_t id = p[2] | (p[3] << 8) | (p[4] << 16) | ((uint32_t)p[5] << 24);
  pci_t *pci = find_pci(id);
  if (!pci) {
    reply_error("unknown_pci");
    return;
  }
  int32_t before = 0;
  dvdnav_get_current_highlight(nav, &before);
  dvdnav_status_t st;
  switch (op) {
    case 1: st = dvdnav_upper_button_select(nav, pci); break;
    case 2: st = dvdnav_lower_button_select(nav, pci); break;
    case 3: st = dvdnav_left_button_select(nav, pci); break;
    case 4: st = dvdnav_right_button_select(nav, pci); break;
    case 5: st = dvdnav_button_activate(nav, pci); break;
    case 6: st = dvdnav_button_select(nav, pci, button); break;
    default: reply_error("bad_op"); return;
  }
  int32_t after = 0;
  dvdnav_get_current_highlight(nav, &after);
  j_begin();
  j("\"ok\":%s,\"op\":%u,\"before\":%d,\"after\":%d", st == DVDNAV_STATUS_OK ? "true" : "false", op,
    before, after);
  if (st != DVDNAV_STATUS_OK) j(",\"detail\":\"%s\"", dvdnav_err_to_string(nav));
  j_end('R');
}

static void cmd_resync(const uint8_t *p, uint32_t len) {
  if (!nav || len < 4) {
    reply_error(nav ? "bad_resync" : "not_open");
    return;
  }
  uint32_t id = p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
  pci_t *want = find_pci(id);
  pci_t *cur = dvdnav_get_current_nav_pci(nav);
  uint32_t pos = 0, length = 0;
  if (!want || !cur || dvdnav_get_position(nav, &pos, &length) != DVDNAV_STATUS_OK) {
    reply_error("no_position");
    return;
  }
  /* The PGC's first sector, from where the reader is now; then the target's offset in it. */
  int64_t pgc_start = (int64_t)cur->pci_gi.nv_pck_lbn - (int64_t)pos;
  int64_t target = (int64_t)want->pci_gi.nv_pck_lbn - pgc_start;
  if (target < 0 || target >= (int64_t)length) {
    reply_error("outside_pgc");
    return;
  }
  dvdnav_status_t st = dvdnav_sector_search(nav, target, SEEK_SET);
  j_begin();
  j("\"ok\":%s,\"target\":%" PRId64 ",\"was\":%u,\"length\":%u", st == DVDNAV_STATUS_OK ? "true" : "false",
    target, pos, length);
  if (st != DVDNAV_STATUS_OK) j(",\"detail\":\"%s\"", dvdnav_err_to_string(nav));
  j_end('R');
}

/* The user operations the disc forbids now: the program chain's and the
   current VOBU's masks together. */
static user_ops_t uops(void) {
  user_ops_t u;
  memset(&u, 0, sizeof u);
  if (nav->vm->state.pgc) u = nav->vm->state.pgc->prohibited_ops;
  pci_t *pci = dvdnav_get_current_nav_pci(nav);
  if (pci) {
    const uint8_t *a = (const uint8_t *)&pci->pci_gi.vobu_uop_ctl;
    uint8_t *b = (uint8_t *)&u;
    for (size_t i = 0; i < sizeof u; i++) b[i] |= a[i];
  }
  return u;
}

/* Is the command forbidden by the disc? */
static int prohibited(char c, const uint8_t *p, uint32_t len) {
  user_ops_t u = uops();
  switch (c) {
    case 'M':
      if (len < 1) return 0;
      switch (p[0]) {
        case DVD_MENU_Title: return u.title_menu_call;
        case DVD_MENU_Root: return u.root_menu_call;
        case DVD_MENU_Subpicture: return u.subpic_menu_call;
        case DVD_MENU_Audio: return u.audio_menu_call;
        case DVD_MENU_Angle: return u.angle_menu_call;
        case DVD_MENU_Part: return u.chapter_menu_call;
        default: return 0;
      }
    case 'U': return u.go_up;
    case 'P': return len >= 1 && (p[0] == 2 ? u.next_pg_search : u.prev_or_top_pg_search);
    case 'A': return u.audio_stream_change;
    case 'L': return u.subpic_stream_change;
    case 'J': return u.time_or_chapter_search;
    case 'V': return u.angle_change;
    default: return 0;
  }
}

/* 'L': op 0 hides subtitles; 1 shows the next stream the program chain has,
   or hides them after its last; 2 shows logical stream `want_stream`. */
static dvdnav_status_t subtitles(uint8_t op, uint8_t want_stream) {
  if (op == 0) return dvdnav_toggle_spu_stream(nav, 0);
  uint16_t sprm2 = nav->vm->state.registers.SPRM[2];
  int on = (sprm2 & 0x40) != 0, cur = sprm2 & 0x3f;
  int first = -1, next = -1;
  for (int i = 0; i < 32; i++) {
    if (dvdnav_get_spu_logical_stream(nav, (uint8_t)i) < 0) continue;
    if (first < 0) first = i;
    if (on && i > cur) { next = i; break; }
  }
  if (first < 0) {
    snprintf(last_error, sizeof last_error, "no_subtitles");
    return DVDNAV_STATUS_ERR;
  }
  if (op == 2) {
    if (want_stream > 31 || dvdnav_get_spu_logical_stream(nav, want_stream) < 0) {
      snprintf(last_error, sizeof last_error, "no_such_stream");
      return DVDNAV_STATUS_ERR;
    }
    if (dvdnav_set_active_stream(nav, want_stream, DVD_SUBTITLE_STREAM) != DVDNAV_STATUS_OK)
      return DVDNAV_STATUS_ERR;
    return dvdnav_toggle_spu_stream(nav, 1);
  }
  int want = on ? next : first;
  if (want < 0) return dvdnav_toggle_spu_stream(nav, 0);
  if (dvdnav_set_active_stream(nav, (uint8_t)want, DVD_SUBTITLE_STREAM) != DVDNAV_STATUS_OK)
    return DVDNAV_STATUS_ERR;
  return dvdnav_toggle_spu_stream(nav, 1);
}

static void cmd_simple(char c, const uint8_t *p, uint32_t len) {
  if (!nav) {
    reply_error("not_open");
    return;
  }
  if (prohibited(c, p, len)) {
    reply_error("uop_prohibited");
    return;
  }
  dvdnav_status_t st = DVDNAV_STATUS_OK;
  switch (c) {
    case 'S': st = dvdnav_still_skip(nav); break;
    case 'W': st = dvdnav_wait_skip(nav); break;
    case 'U': st = dvdnav_go_up(nav); break;
    case 'M':
      if (len < 1) { reply_error("bad_menu"); return; }
      st = dvdnav_menu_call(nav, (DVDMenuID_t)p[0]);
      break;
    case 'T': {
      if (len < 4) { reply_error("bad_play"); return; }
      int32_t title = p[0] | (p[1] << 8), part = p[2] | (p[3] << 8);
      st = part ? dvdnav_part_play(nav, title, part) : dvdnav_title_play(nav, title);
      break;
    }
    case 'P': /* chapter: 0 previous, 1 top of this one, 2 next */
      if (len < 1) { reply_error("bad_chapter"); return; }
    {
      /* Previous in the first chapter restarts it, as players do; libdvdnav's
         previous-program search does nothing there, and no jump follows. */
      int32_t t = 0, part = 0;
      dvdnav_current_title_info(nav, &t, &part);
      uint8_t to = (p[0] == 0 && part <= 1) ? 1 : p[0];
      st = to == 0 ? dvdnav_prev_pg_search(nav) : to == 1 ? dvdnav_top_pg_search(nav)
                                                       : dvdnav_next_pg_search(nav);
      break;
    }
    case 'A': /* logical audio stream, as the remote's audio button selects it */
      if (len < 1) { reply_error("bad_audio"); return; }
      st = dvdnav_set_active_stream(nav, p[0], DVD_AUDIO_STREAM);
      break;
    case 'L':
      if (len < 1 || p[0] > 2 || (p[0] == 2 && len < 2)) { reply_error("bad_subtitles"); return; }
      if (!dvdnav_is_domain_vts(nav)) { reply_error("not_in_title"); return; }
      last_error[0] = 0;
      st = subtitles(p[0], p[0] == 2 ? p[1] : 0);
      if (st != DVDNAV_STATUS_OK && !strcmp(last_error, "no_subtitles")) { reply_error("no_subtitles"); return; }
      if (st != DVDNAV_STATUS_OK && !strcmp(last_error, "no_such_stream")) { reply_error("no_such_stream"); return; }
      break;
    case 'J': {
      if (len < 8) { reply_error("bad_seek"); return; }
      uint64_t t = 0;
      for (int i = 7; i >= 0; i--) t = (t << 8) | p[i];
      if (!dvdnav_is_domain_vts(nav)) { reply_error("not_in_title"); return; }
      st = dvdnav_time_search(nav, t);
      break;
    }
    case 'V': {
      if (len < 1) { reply_error("bad_angle"); return; }
      int32_t cur = 1, num = 1;
      dvdnav_get_angle_info(nav, &cur, &num);
      if (num <= 1) { reply_error("no_angles"); return; }
      st = dvdnav_angle_change(nav, p[0] ? p[0] : (cur % num) + 1);
      break;
    }
    case 'Q': break;
    default: reply_error("bad_command"); return;
  }
  j_begin();
  j("\"ok\":%s,", st == DVDNAV_STATUS_OK ? "true" : "false");
  if (st != DVDNAV_STATUS_OK) j("\"detail\":\"%s\",", dvdnav_err_to_string(nav));
  j_position();
  j_registers();
  j_end('R');
}

#ifndef SEEVUE_WORKER_VERSION
#define SEEVUE_WORKER_VERSION "dev"
#endif

int main(void) {
  static uint8_t payload[256];
  /* The hello, once, before any command is read. */
  static const char hello[] = "{\"protocol\":7,\"worker\":\"navd\",\"version\":\"" SEEVUE_WORKER_VERSION "\"}";
  frame('H', hello, (uint32_t)(sizeof hello - 1));
  for (;;) {
    uint8_t head[5];
    if (read_all(head, 5) < 0) break;
    uint32_t total = head[0] | (head[1] << 8) | (head[2] << 16) | ((uint32_t)head[3] << 24);
    if (total < 1 || total - 1 > sizeof payload) {
      fprintf(stderr, "navd: bad frame length %u\n", total);
      return 2;
    }
    uint32_t len = total - 1;
    if (len > 0 && read_all(payload, len) < 0) break;
    char c = (char)head[4];
    switch (c) {
      case 'O': cmd_open(payload, len); break;
      case 'N': cmd_next(payload, len); break;
      case 'B': cmd_button(payload, len); break;
      case 'R': cmd_resync(payload, len); break;
      case 'X': j_begin(); j("\"ok\":true"); j_end('R'); goto quit;
      default: cmd_simple(c, payload, len); break;
    }
  }
quit:
  if (nav) dvdnav_close(nav);
  return 0;
}
