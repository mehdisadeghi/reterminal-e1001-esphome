#pragma once

// Photo dial faces, two optional sources with the card on top:
//   1. /<city>.png on a mounted SD card (override)
//   2. <city>.png beside the device yaml, baked in at build time by
//      tools/gen_dial_images.py (dial_images.h)
// city = the zone's last IANA segment, lowercased (Europe/Berlin ->
// berlin.png). Either way the face is scaled to the dial's diameter and
// Floyd-Steinberg dithered to 1-bit at draw time, cached per (city,
// diameter). The draw path never mounts the card itself — it only reads
// when boot or sd_check already mounted one (no power-up races); a card
// insert re-evaluates via dial_images_reset().
//
// SD PNGs are streamed through pngle; inflate comes from the ESP32 ROM's
// miniz. pngle eats only whole syntactic units, so leftovers of a read
// are carried into the next one. Interlaced PNGs are rejected (their rows
// arrive out of order).

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "dial_images.h"
#include "pure.h"

#ifndef RETERMINAL_HOST_TEST
#include <SD.h>

#include "device.h"
#include "pngle.h"
#endif

namespace reterminal {

struct DialFace {
  char key[24];
  int d;          // diameter the bits were dithered for
  uint8_t *bits;  // null = no face for this key (cached miss)
};

static DialFace dial_faces[MAX_ZONES];

inline void dial_images_reset() {
  for (auto &e : dial_faces) {
    free(e.bits);
    e.bits = nullptr;
    e.key[0] = 0;
  }
}

// --- embedded source ---------------------------------------------------------

inline uint8_t *dither_face(const DialSrc &s, int d) {
  int stride = (d + 7) / 8;
  uint8_t *bits = (uint8_t *) calloc((size_t) stride * d, 1);
  int16_t *cur = (int16_t *) calloc(d + 2, sizeof(int16_t));
  int16_t *nxt = (int16_t *) calloc(d + 2, sizeof(int16_t));
  if (bits == nullptr || cur == nullptr || nxt == nullptr) {
    free(bits);
    free(cur);
    free(nxt);
    return nullptr;
  }
  for (int ty = 0; ty < d; ty++) {
    const uint8_t *row = s.gray + (size_t) ((int64_t) ty * s.side / d) * s.side;
    memset(nxt, 0, (d + 2) * sizeof(int16_t));
    for (int tx = 0; tx < d; tx++) {
      int g = row[(int) ((int64_t) tx * s.side / d)];
      int old = g + cur[tx + 1];
      int e;
      if (old < 128) {
        bits[ty * stride + (tx >> 3)] |= 0x80 >> (tx & 7);
        e = old;
      } else {
        e = old - 255;
      }
      cur[tx + 2] += e * 7 / 16;
      nxt[tx] += e * 3 / 16;
      nxt[tx + 1] += e * 5 / 16;
      nxt[tx + 2] += e / 16;
    }
    int16_t *t = cur;
    cur = nxt;
    nxt = t;
  }
  free(cur);
  free(nxt);
  return bits;
}

inline uint8_t *embedded_face(const char *key, int d) {
  for (int i = 0; i < DIAL_SRC_COUNT; i++)
    if (strcmp(DIAL_SRCS[i].name, key) == 0)
      return dither_face(DIAL_SRCS[i], d);
  return nullptr;
}

// --- SD override -------------------------------------------------------------

#ifndef RETERMINAL_HOST_TEST

struct DialDecode {
  int d, stride;
  uint8_t *bits;
  uint32_t src_w, src_h;
  int m, offx, offy;  // centered square crop of the source
  uint8_t *row;       // one source row, gray
  int cur_y;          // source row being filled
  int next_ty;        // next target row to dither
  int16_t *err0, *err1;
  bool fail;
};

inline int di_src_row(DialDecode *s, int ty) { return s->offy + (int) ((int64_t) ty * s->m / s->d); }

// Dither every target row whose nearest source row is sy. Brightness and
// contrast are lifted first (x1.18 / x1.15, the values the embedded
// pipeline bakes in) — 1-bit e-paper buries midtones otherwise.
inline void di_emit_rows(DialDecode *s, int sy) {
  while (s->next_ty < s->d && di_src_row(s, s->next_ty) == sy) {
    int ty = s->next_ty++;
    int16_t *cur = s->err0, *nxt = s->err1;
    memset(nxt, 0, (s->d + 2) * sizeof(int16_t));
    for (int tx = 0; tx < s->d; tx++) {
      int sx = s->offx + (int) ((int64_t) tx * s->m / s->d);
      int g = s->row[sx] * 118 / 100;
      if (g > 255)
        g = 255;
      g = (g - 128) * 115 / 100 + 128;
      if (g < 0)
        g = 0;
      else if (g > 255)
        g = 255;
      int old = g + cur[tx + 1];
      int e;
      if (old < 128) {
        s->bits[ty * s->stride + (tx >> 3)] |= 0x80 >> (tx & 7);
        e = old;
      } else {
        e = old - 255;
      }
      cur[tx + 2] += e * 7 / 16;
      nxt[tx] += e * 3 / 16;
      nxt[tx + 1] += e * 5 / 16;
      nxt[tx + 2] += e / 16;
    }
    s->err0 = nxt;
    s->err1 = cur;
  }
}

inline void di_init(pngle_t *p, uint32_t w, uint32_t h) {
  auto *s = (DialDecode *) pngle_get_user_data(p);
  if (pngle_get_ihdr(p)->interlace != 0) {  // Adam7 rows arrive out of order
    s->fail = true;
    return;
  }
  s->src_w = w;
  s->src_h = h;
  s->m = (int) (w < h ? w : h);
  s->offx = ((int) w - s->m) / 2;
  s->offy = ((int) h - s->m) / 2;
  s->row = (uint8_t *) malloc(w);
  if (s->row == nullptr)
    s->fail = true;
}

inline void di_draw(pngle_t *p, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t rgba[4]) {
  auto *s = (DialDecode *) pngle_get_user_data(p);
  if (s->fail)
    return;
  if ((int) y != s->cur_y) {
    if (s->cur_y >= 0)
      di_emit_rows(s, s->cur_y);
    s->cur_y = (int) y;
  }
  int a = rgba[3];
  int g = (rgba[0] * 299 + rgba[1] * 587 + rgba[2] * 114) / 1000;
  g = (g * a + 255 * (255 - a)) / 255;  // composite over paper
  for (uint32_t i = 0; i < w && x + i < s->src_w; i++)
    s->row[x + i] = (uint8_t) g;
  (void) h;
}

inline uint8_t *sd_face(const char *key, int d) {
  char file[32];
  snprintf(file, sizeof(file), "/%s.png", key);
  File f = SD.open(file);
  if (!f)
    return nullptr;
  int stride = (d + 7) / 8;
  DialDecode s = {};
  s.d = d;
  s.stride = stride;
  s.cur_y = -1;
  s.bits = (uint8_t *) calloc((size_t) stride * d, 1);
  s.err0 = (int16_t *) calloc(d + 2, sizeof(int16_t));
  s.err1 = (int16_t *) calloc(d + 2, sizeof(int16_t));
  pngle_t *p = pngle_new();
  bool ok = s.bits != nullptr && s.err0 != nullptr && s.err1 != nullptr && p != nullptr;
  if (ok) {
    pngle_set_user_data(p, &s);
    pngle_set_init_callback(p, di_init);
    pngle_set_draw_callback(p, di_draw);
    uint8_t buf[1024];
    size_t have = 0;
    while (ok) {
      int n = f.read(buf + have, sizeof(buf) - have);
      if (n <= 0)
        break;
      have += (size_t) n;
      int ate = pngle_feed(p, buf, have);
      if (ate < 0 || s.fail || (ate == 0 && have == sizeof(buf))) {
        ok = false;
        break;
      }
      memmove(buf, buf + ate, have - ate);
      have -= (size_t) ate;
    }
    if (ok && s.cur_y >= 0)
      di_emit_rows(&s, s.cur_y);
    ok = ok && !s.fail && s.next_ty == s.d;
  }
  if (p != nullptr)
    pngle_destroy(p);
  free(s.row);
  free(s.err0);
  free(s.err1);
  f.close();
  if (!ok) {
    free(s.bits);
    ESP_LOGW(TAG, "dial image %s decode failed", file);
    return nullptr;
  }
  ESP_LOGI(TAG, "dial image %s decoded at d=%d", file, d);
  return s.bits;
}

#endif  // RETERMINAL_HOST_TEST

// The face bits for a zone's dial at radius r, or null when neither the
// card nor the firmware carries an image for it.
inline const uint8_t *zone_image(const Zone &z, int r) {
  if (z.iana[0] == 0)
    return nullptr;
  char key[24];
  const char *seg = strrchr(z.iana, '/');
  seg = seg != nullptr ? seg + 1 : z.iana;
  size_t o = 0;
  for (const char *p = seg; *p != 0 && o + 1 < sizeof(key); p++)
    key[o++] = (char) tolower((unsigned char) *p);
  key[o] = 0;
  int d = 2 * r;
  DialFace *slot = nullptr;
  for (auto &e : dial_faces)
    if (e.key[0] != 0 && strcmp(e.key, key) == 0) {
      slot = &e;
      break;
    }
  if (slot != nullptr && slot->d == d)
    return slot->bits;
  if (slot == nullptr)
    for (auto &e : dial_faces)
      if (e.key[0] == 0) {
        slot = &e;
        break;
      }
  if (slot == nullptr) {
    dial_images_reset();
    slot = &dial_faces[0];
  }
  uint8_t *bits = nullptr;
#ifndef RETERMINAL_HOST_TEST
  if (sd_mounted)
    bits = sd_face(key, d);
#endif
  if (bits == nullptr)
    bits = embedded_face(key, d);
  snprintf(slot->key, sizeof(slot->key), "%s", key);
  free(slot->bits);
  slot->d = d;
  slot->bits = bits;
  return bits;
}

}  // namespace reterminal
