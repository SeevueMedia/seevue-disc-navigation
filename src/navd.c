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
 *                 so a following menu call saves the displayed position for resume; it jumps
 *                 to the VOBU's own cell (in an angle block, its angle's), by sector. The jump
 *                 waits for the next read or the next command the disc allows there (masks
 *                 at that VOBU); a refused command, a button, play or seek drops it
 *   After a stop (a title that ends with nowhere to go) the next read, play or menu call
 *   starts the disc again at First Play with the player's settings kept.
 *     'L' subtitles u8 op: 0 hide, 1 the next stream the title has (after the last: hide),
 *                 2 show logical stream u8 n (protocol 7; "no_such_stream" when the
 *                 program chain lacks it)
 *     'J' seek    u64 90 kHz time in the current program chain: the VOBU starting at or before
 *                 it, found by the VOBUs' own times (0.8.3)
 *     'V' angle   u8 angle (1-based; 0 the next one)
 *   On entering a title's program chain (protocol 7), before its first "cell" event:
 *     {"ev":"title_info","title":t,"pgc":n,"length_90k":L,"chapters_90k":[start, ...],
 *      "audio":[{"logical":i,"lang":"en","format":f,"channels":c,"ext":e}, ...],
 *      "spu":[{"logical":i,"lang":"en","ext":e}, ...]}
 *   Times are in the program chain (the time 'J' seeks to); a chapter is a program.
 *   A "nav" event in a title adds "pgc_90k" (protocol 7): its VOBU's time in the
 *   program chain, where a seek landed.
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
#include <dvdread/nav_read.h>
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
/* Each stored PCI's VOBU: its VOB and cell ids (from the DSI) and the program chain it was read in,
   so a resync finds the VOBU's own cell (in an angle block, cells' sector ranges overlap). */
static uint16_t pci_vob[PCI_RING];
static uint8_t pci_cell[PCI_RING];
static const pgc_t *pci_pgc[PCI_RING];
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

static uint32_t store_pci(const pci_t *pci, const dsi_t *dsi) {
  uint32_t id = next_pci_id++;
  uint32_t slot = id % PCI_RING;
  pci_ring[slot] = *pci;
  pci_ids[slot] = id;
  pci_vob[slot] = dsi->dsi_gi.vobu_vob_idn;
  pci_cell[slot] = dsi->dsi_gi.vobu_c_idn;
  pci_pgc[slot] = nav->vm->state.pgc;
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
  /* Protocol 7: this VOBU's time in the title's program chain (a seek lands here). */
  if (dvdnav_is_domain_vts(nav)) j("\"pgc_90k\":%" PRId64 ",", dvdnav_get_current_time(nav));
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

/* A resync's jump, until the next read or allowed command (see cmd_resync). */
static struct {
  int active;
  int cell;
  uint32_t block;
  pci_t pci;
  const pgc_t *pgc;
} pending;

static void drop_pending(void) { pending.active = 0; }

/* Moves the VM to the pending VOBU: its cell (in an angle block, the angle register follows it, so
   the next angle counts from the picture on screen; entering a later angle's cell directly only makes
   libdvdnav log that it did not enter at the block's first cell) and its sector. A plain hop, not a
   seek: after a seek in a multi-angle title libdvdnav skips the VOBU it lands on. */
static void apply_pending(void) {
  if (!pending.active) return;
  pending.active = 0;
  dvd_state_t *state = &nav->vm->state;
  if (state->pgc != pending.pgc || nav->vm->stopped) return;
  const pgc_t *pgc = state->pgc;
  pthread_mutex_lock(&nav->vm_lock);
  if (pgc->cell_playback[pending.cell - 1].block_type == BLOCK_TYPE_ANGLE_BLOCK) {
    int block_first = pending.cell;
    while (block_first > 1 && pgc->cell_playback[block_first - 1].block_mode != BLOCK_MODE_FIRST_CELL)
      block_first--;
    state->AGL_REG = pending.cell - block_first + 1;
  }
  vm_jump_cell_block(nav->vm, pending.cell, (int)pending.block);
  if (state->cellN == pending.cell) {
    nav->cur_cell_time = 0;
    nav->vm->hop_channel++;
  }
  pthread_mutex_unlock(&nav->vm_lock);
}

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
  /* What libdvdnav logged while opening is not damage (an image read through the read service
     always logs "dvd_read_name failed"): a later stop's detail names only what came after. */
  last_error[0] = 0;
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
  apply_pending();
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
        if (pci && dsi) event_nav(pci, dsi, store_pci(pci, dsi));
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
  drop_pending(); /* a button acts on its own VOBU's highlight */
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
  drop_pending();
  uint32_t id = p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
  pci_t *want = find_pci(id);
  dvd_state_t *state = &nav->vm->state;
  uint32_t slot = id % PCI_RING;
  /* Not in a still, and not once libdvdnav has stopped the VM (the next read starts it again). */
  if (!want || !state->pgc || nav->position_current.still || !nav->started || nav->vm->stopped) {
    reply_error("no_position");
    return;
  }
  /* The VOBU's own cell in the current program chain, by its VOB and cell ids, and a jump to its
     sector in it. libdvdnav's sector search cannot do this: its offsets count a program's cells
     but skip an angle block's later angles, while positions count them all, so after an angle
     block (or in a later angle) an offset lands in the wrong place. */
  const pgc_t *pgc = state->pgc;
  uint32_t lbn = want->pci_gi.nv_pck_lbn;
  int cell = 0;
  if (pci_pgc[slot] == pgc) {
    for (int c = 1; c <= pgc->nr_of_cells && !cell; c++) {
      const cell_position_t *at = &pgc->cell_position[c - 1];
      const cell_playback_t *pb = &pgc->cell_playback[c - 1];
      if (at->vob_id_nr == pci_vob[slot] && at->cell_nr == pci_cell[slot] &&
          lbn >= pb->first_sector && lbn <= pb->last_sector)
        cell = c;
    }
  }
  if (!cell) {
    reply_error("outside_pgc");
    return;
  }
  /* The jump waits for what follows: the next read, or the next command the disc allows at the
     picture on screen (its masks are that VOBU's, not the reader's). A command the disc forbids
     drops it, so a refused key leaves the reader where it was and nothing restarts. */
  pending.active = 1;
  pending.cell = cell;
  pending.block = lbn - pgc->cell_playback[cell - 1].first_sector;
  pending.pci = *want;
  pending.pgc = pgc;
  j_begin();
  j("\"ok\":true,\"cell\":%d,\"was_cell\":%d,\"lbn\":%u", cell, state->cellN, lbn);
  j_end('R');
}

/* The user operations the disc forbids now: the program chain's and the
   current VOBU's masks together (after a resync, the picture on screen's). */
static user_ops_t uops(void) {
  user_ops_t u;
  memset(&u, 0, sizeof u);
  if (nav->vm->state.pgc) u = nav->vm->state.pgc->prohibited_ops;
  pci_t *pci = pending.active ? &pending.pci : dvdnav_get_current_nav_pci(nav);
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
/* A VOBU's time in its cell (90 kHz): its NAV pack's cell elapsed time, read from the title's VOBs
   (one sector). -1 when the sector does not read or is not that VOBU's NAV pack. */
static int64_t vobu_cell_time(uint32_t sector) {
  static uint8_t buf[SECTOR];
  if (DVDReadBlocks(nav->file, (int)sector, 1, buf) != 1) return -1;
  /* A NAV pack: pack header, system header, then the PCI and DSI private stream 2 packets. */
  if (buf[0] || buf[1] || buf[2] != 1 || buf[3] != 0xba) return -1;
  if (buf[0x400] || buf[0x401] || buf[0x402] != 1 || buf[0x403] != 0xbf || buf[0x406] != 1) return -1;
  dsi_t dsi;
  navRead_DSI(&dsi, buf + DSI_START_BYTE);
  if (dsi.dsi_gi.nv_pck_lbn != sector) return -1;
  return dvdnav_convert_time(&dsi.dsi_gi.c_eltm);
}

/* 'J': a seek to t (90 kHz, in the program chain). libdvdnav's dvdnav_time_search finds the cell,
   then guesses the sector by interpolating over the cell's bytes, which on a variable-bitrate film
   lands seconds away (seevue#1555: 7 s early). navd finds the cell the same way, then the VOBU
   itself: the cell's VOBUs from the title's address map, bisected by each VOBU's own time (one
   NAV pack read each, about a dozen per seek), landing on the last VOBU that starts at or before
   t. An angle block, a cell the map does not cover in order, or a NAV pack that does not read
   falls back to libdvdnav's search. */
static dvdnav_status_t seek_time(uint64_t t) {
  dvd_state_t *state = &nav->vm->state;
  const pgc_t *pgc = state->pgc;
  if (!pgc || nav->position_current.still || !nav->file || !nav->vm->vtsi ||
      !nav->vm->vtsi->vts_vobu_admap || state->domain != DVD_DOMAIN_VTSTitle)
    return dvdnav_time_search(nav, t);
  int first = 1, last = pgc->nr_of_cells;
  if (!nav->pgc_based) {
    first = pgc->program_map[state->pgN - 1];
    if (state->pgN < pgc->nr_of_programs) last = pgc->program_map[state->pgN] - 1;
  }
  uint64_t left = t;
  int cell = 0;
  for (int c = first; c <= last && !cell; c++) {
    const cell_playback_t *pb = &pgc->cell_playback[c - 1];
    if (pb->block_type == BLOCK_TYPE_ANGLE_BLOCK && pb->block_mode != BLOCK_MODE_FIRST_CELL) continue;
    uint64_t length = (uint64_t)dvdnav_convert_time(&pb->playback_time);
    if (left >= length) left -= length;
    else cell = c;
  }
  if (!cell) return dvdnav_time_search(nav, t);
  const cell_playback_t *pb = &pgc->cell_playback[cell - 1];
  if (pb->block_type == BLOCK_TYPE_ANGLE_BLOCK) return dvdnav_time_search(nav, t);
  /* The cell's VOBUs: a run of the address map from its first sector, in increasing order. */
  const vobu_admap_t *map = nav->vm->vtsi->vts_vobu_admap;
  uint32_t entries = (map->last_byte + 1 - VOBU_ADMAP_SIZE) / VOBU_ADMAP_SIZE;
  uint32_t lo = 0;
  while (lo < entries && map->vobu_start_sectors[lo] != pb->first_sector) lo++;
  if (lo == entries) return dvdnav_time_search(nav, t);
  uint32_t hi = lo + 1;
  while (hi < entries && map->vobu_start_sectors[hi] <= pb->last_sector) {
    if (map->vobu_start_sectors[hi] <= map->vobu_start_sectors[hi - 1]) return dvdnav_time_search(nav, t);
    hi++;
  }
  /* The last VOBU in [lo, hi) starting at or before `left`; the cell's first starts at 0. */
  uint32_t a = lo, b = hi;
  while (b - a > 1) {
    uint32_t m = a + (b - a) / 2;
    int64_t at = vobu_cell_time(map->vobu_start_sectors[m]);
    if (at < 0) return dvdnav_time_search(nav, t);
    if ((uint64_t)at <= left) a = m;
    else b = m;
  }
  pthread_mutex_lock(&nav->vm_lock);
  int ok = vm_jump_cell_block(nav->vm, cell, (int)(map->vobu_start_sectors[a] - pb->first_sector));
  if (ok) {
    nav->cur_cell_time = 0;
    nav->vm->hop_channel += HOP_SEEK;
  }
  pthread_mutex_unlock(&nav->vm_lock);
  return ok ? DVDNAV_STATUS_OK : dvdnav_time_search(nav, t);
}

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
    drop_pending();
    reply_error("uop_prohibited");
    return;
  }
  dvdnav_status_t st = DVDNAV_STATUS_OK;
  switch (c) {
    case 'S': apply_pending(); st = dvdnav_still_skip(nav); break;
    case 'W': apply_pending(); st = dvdnav_wait_skip(nav); break;
    case 'U': apply_pending(); st = dvdnav_go_up(nav); break;
    case 'M':
      if (len < 1) { drop_pending(); reply_error("bad_menu"); return; }
      apply_pending();
      st = dvdnav_menu_call(nav, (DVDMenuID_t)p[0]);
      break;
    case 'T': {
      drop_pending();
      if (len < 4) { reply_error("bad_play"); return; }
      int32_t title = p[0] | (p[1] << 8), part = p[2] | (p[3] << 8);
      st = part ? dvdnav_part_play(nav, title, part) : dvdnav_title_play(nav, title);
      break;
    }
    case 'P': /* chapter: 0 previous, 1 top of this one, 2 next */
      if (len < 1) { drop_pending(); reply_error("bad_chapter"); return; }
      apply_pending();
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
      if (len < 1) { drop_pending(); reply_error("bad_audio"); return; }
      apply_pending();
      st = dvdnav_set_active_stream(nav, p[0], DVD_AUDIO_STREAM);
      break;
    case 'L':
      if (len < 1 || p[0] > 2 || (p[0] == 2 && len < 2)) { drop_pending(); reply_error("bad_subtitles"); return; }
      if (!dvdnav_is_domain_vts(nav)) { drop_pending(); reply_error("not_in_title"); return; }
      apply_pending();
      last_error[0] = 0;
      st = subtitles(p[0], p[0] == 2 ? p[1] : 0);
      if (st != DVDNAV_STATUS_OK && !strcmp(last_error, "no_subtitles")) { reply_error("no_subtitles"); return; }
      if (st != DVDNAV_STATUS_OK && !strcmp(last_error, "no_such_stream")) { reply_error("no_such_stream"); return; }
      break;
    case 'J': {
      drop_pending(); /* a seek goes somewhere else */
      if (len < 8) { reply_error("bad_seek"); return; }
      uint64_t t = 0;
      for (int i = 7; i >= 0; i--) t = (t << 8) | p[i];
      if (!dvdnav_is_domain_vts(nav)) { reply_error("not_in_title"); return; }
      st = seek_time(t);
      break;
    }
    case 'V': {
      if (len < 1) { drop_pending(); reply_error("bad_angle"); return; }
      int32_t cur = 1, num = 1;
      dvdnav_get_angle_info(nav, &cur, &num);
      if (num <= 1) { drop_pending(); reply_error("no_angles"); return; }
      apply_pending();
      dvdnav_get_angle_info(nav, &cur, &num);
      st = dvdnav_angle_change(nav, p[0] ? p[0] : (cur % num) + 1);
      break;
    }
    case 'Q': break;
    default: drop_pending(); reply_error("bad_command"); return;
  }
  j_begin();
  j("\"ok\":%s,", st == DVDNAV_STATUS_OK ? "true" : "false");
  if (st != DVDNAV_STATUS_OK) j("\"detail\":\"%s\",", dvdnav_err_to_string(nav));
  j_position();
  j_registers();
  j_end('R');
}

/* A stopped VM (the title ended with nowhere to go) is restarted by libdvdnav's next read, title
   play or menu call with vm_reset(NULL, NULL, NULL), which forgets the stream callbacks the image is
   read through (libdvdread then reads through NULL ones and navd crashes) and the player's settings
   (languages, region, parental level, display). navd restarts it as a player does: the disc's own
   state cleared, the player's kept, and libdvdnav finds the VM not stopped, so it plays First Play
   on the open disc. */
static void before_restart(void) {
  if (!nav || nav->started || !nav->vm->stopped) return;
  dvd_state_t *s = &nav->vm->state;
  memset(s->registers.GPRM, 0, sizeof s->registers.GPRM);
  memset(s->registers.GPRM_mode, 0, sizeof s->registers.GPRM_mode);
  memset(s->registers.GPRM_time, 0, sizeof s->registers.GPRM_time);
  s->AST_REG = 15;
  s->SPST_REG = 62;
  s->AGL_REG = 1;
  s->TTN_REG = 1;
  s->VTS_TTN_REG = 1;
  s->PTTN_REG = 1;
  s->HL_BTNN_REG = 1 << 10;
  s->rsm_vtsN = 0;
  s->rsm_cellN = 0;
  s->rsm_blockN = 0;
  nav->vm->stopped = 0;
  last_error[0] = 0;
  drop_pending();
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
    if (c != 'O' && c != 'Q' && c != 'X' && c != 'R') before_restart();
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
