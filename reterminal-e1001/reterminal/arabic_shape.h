#pragma once

// Runtime Arabic/Persian shaper for user-entered zone labels: contextual
// form selection + lam-alef ligatures + visual reordering. Runs once per
// config change (parse time), never per frame. The form table is generated
// from arabic-reshaper's data (arabic_forms.h), so runtime shaping agrees
// glyph for glyph with the build-time pipeline — the host tests cross-check
// against generated strings.
//
// Scope: single-script labels. A string with no Arabic letters is copied
// through unchanged; mixed-script or digit-bearing labels are not reordered
// per bidi (labels should be one word or phrase in one script).

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "arabic_forms.h"

namespace reterminal {

inline const ArabicForms *arabic_form(uint32_t cp) {
  for (int i = 0; i < ARABIC_FORM_COUNT; i++)
    if (ARABIC_FORMS[i].base == cp)
      return &ARABIC_FORMS[i];
  return nullptr;
}

inline uint32_t utf8_next(const char *&p) {
  uint8_t c = (uint8_t) *p;
  if (c < 0x80) {
    p += 1;
    return c;
  }
  if ((c >> 5) == 0x6) {
    uint32_t v = ((uint32_t) (c & 0x1F) << 6) | ((uint8_t) p[1] & 0x3F);
    p += 2;
    return v;
  }
  if ((c >> 4) == 0xE) {
    uint32_t v = ((uint32_t) (c & 0x0F) << 12) | (((uint32_t) ((uint8_t) p[1] & 0x3F)) << 6) |
                 ((uint8_t) p[2] & 0x3F);
    p += 3;
    return v;
  }
  uint32_t v = ((uint32_t) (c & 0x07) << 18) | (((uint32_t) ((uint8_t) p[1] & 0x3F)) << 12) |
               (((uint32_t) ((uint8_t) p[2] & 0x3F)) << 6) | ((uint8_t) p[3] & 0x3F);
  p += 4;
  return v;
}

inline size_t utf8_put(char *out, uint32_t cp) {
  if (cp < 0x80) {
    out[0] = (char) cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (char) (0xC0 | (cp >> 6));
    out[1] = (char) (0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char) (0xE0 | (cp >> 12));
    out[1] = (char) (0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char) (0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (char) (0xF0 | (cp >> 18));
  out[1] = (char) (0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char) (0x80 | ((cp >> 6) & 0x3F));
  out[3] = (char) (0x80 | (cp & 0x3F));
  return 4;
}

// Shape `in` into visual-order presentation forms in `out` (truncating at
// character boundaries if needed). ZWNJ breaks joining and is dropped.
inline void shape_arabic(const char *in, char *out, size_t n) {
  constexpr int MAXCP = 40;
  uint32_t cps[MAXCP];
  int cnt = 0;
  bool any = false;
  for (const char *p = in; *p != 0 && cnt < MAXCP;) {
    cps[cnt] = utf8_next(p);
    if (arabic_form(cps[cnt]) != nullptr)
      any = true;
    cnt++;
  }
  if (!any) {  // pure Latin (or unknown script): copy through unchanged
    snprintf(out, n, "%s", in);
    return;
  }
  uint32_t vis[MAXCP];
  int vn = 0;
  bool prev_fwd = false;  // previous letter connects forward
  for (int i = 0; i < cnt; i++) {
    uint32_t cp = cps[i];
    if (cp == 0x200C) {  // ZWNJ: joining barrier, no glyph
      prev_fwd = false;
      continue;
    }
    const ArabicForms *f = arabic_form(cp);
    if (f == nullptr) {
      vis[vn++] = cp;
      prev_fwd = false;
      continue;
    }
    if (cp == 0x0644 && i + 1 < cnt) {  // lam + alef -> ligature
      const LamAlef *la = nullptr;
      for (int k = 0; k < LAM_ALEF_COUNT; k++)
        if (LAM_ALEF[k].alef == cps[i + 1])
          la = &LAM_ALEF[k];
      if (la != nullptr) {
        vis[vn++] = prev_fwd ? la->fin : la->iso;
        prev_fwd = false;  // alef never connects forward
        i++;
        continue;
      }
    }
    const ArabicForms *nf = nullptr;
    if (i + 1 < cnt && cps[i + 1] != 0x200C)
      nf = arabic_form(cps[i + 1]);
    bool back = prev_fwd && f->fin != 0;
    bool fwd = (f->ini != 0 || f->med != 0) && nf != nullptr && nf->fin != 0;
    uint32_t sel;
    if (back && fwd)
      sel = f->med != 0 ? f->med : (f->fin != 0 ? f->fin : f->iso);
    else if (back)
      sel = f->fin != 0 ? f->fin : f->iso;
    else if (fwd)
      sel = f->ini != 0 ? f->ini : f->iso;
    else
      sel = f->iso;
    vis[vn++] = sel;
    prev_fwd = fwd;
  }
  // visual order: reverse the RTL run
  size_t o = 0;
  for (int i = vn - 1; i >= 0; i--) {
    char tmp[4];
    size_t len = utf8_put(tmp, vis[i]);
    if (o + len + 1 > n)
      break;
    memcpy(out + o, tmp, len);
    o += len;
  }
  out[o] = 0;
}

}  // namespace reterminal
