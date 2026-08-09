# 0006 — Status-bar type is rasterized on the device

**Status:** Accepted — supersedes [0003](0003-compiled-font-ladder.md)

## Context

[0003](0003-compiled-font-ladder.md) offered five compiled sizes because
ESPHome bakes fonts at build time. The granularity was coarse and the reason
was flash: ~10 KB of per-glyph metadata per rung, before any bitmaps.

But the project already carries the outlines — `vazirmatn.ttf` is in the
repo, and Inter is fetched at build time. What was missing was a rasterizer.
Three options:

1. **Upscale existing bitmaps.** Nearly free, and unusable: integer
   multiples only, and fractional scaling makes stems 1 px in one place and
   2 px in another *within the same letter*.
2. **Downscale from one large master.** Degrades the small sizes, which are
   the common case. Rejected.
3. **Rasterize outlines on demand** with `stb_truetype`.

The CPU objection does not survive contact with the numbers: a few
milliseconds per glyph, against an ePaper refresh measured in seconds.

## Decision

Carry subset outlines and rasterize on demand.

- `stb_truetype.h` is vendored beside `pngle.[ch]` — the same pattern the
  project already uses for a single-file permissive C library.
- `tools/gen_ttf.py` subsets each face to the glyphs the UI can draw and
  **drops GSUB/GPOS**, which is nearly free here: Persian is pre-shaped into
  presentation forms at build time, so the shaping tables were dead weight.
  Result: 8.1 KB (Inter Bold, 79 glyphs) + 24.2 KB (Vazirmatn, 284).
- `TtfFont` implements ESPHome's `BaseFont` — a two-method interface,
  `print` and `measure`. `measure` is what `get_text_bounds` calls, so
  [0004](0004-status-bar-give-way.md)'s fitting pass keeps working untouched.
- **No glyph cache.** `measure()` reads advances from `hmtx` without
  rasterizing, which is what makes the give-way pass cheap enough to run
  repeatedly; `print()` rasterizes and frees per glyph.

Scaling uses `stbtt_ScaleForMappingEmToPixels`, **not**
`stbtt_ScaleForPixelHeight`. The latter normalizes ascent-to-descent rather
than the em square: it would have rendered Inter 21% and Vazirmatn 56% small
at the same nominal size, so one setting would have produced visibly
different sizes in the two languages. Em-to-pixels matches what ESPHome's
`size:` means, so a number keeps meaning what it did.

## Consequences

Any size from 14–48 px, step 1 — and flash went **down**:

| | flash |
|---|---|
| before any size setting | 78.2% |
| compiled ladder (0003) | 80.7% |
| rasterized | **78.6%** |

65 KB below the ladder, 13 KB above having no setting at all.

The cost is hinting. `stb_truetype` ignores it, so these faces are for
display sizes; the small compiled faces still render body text, where
hinting is what keeps 1–2 px stems crisp. The status bar's own default
(20 px) is now rasterized and slightly softer than the compiled face it
replaced — accepted because a rendering discontinuity between 20 and 21 px
would be worse than a uniformly softer bar.

Verified on host: all 79 Latin and 284 Persian glyphs survive subsetting —
including the U+FB50–FEFF presentation forms, where a dropped glyph would
have drawn *nothing*, silently — with sane metrics and real ink across the
range.

## Revisit if

- The unhinted default looks wrong on the panel. Pinning exactly the default
  size back to the compiled face is a one-line change.
- The other roles migrate: ~408 KB of compiled faces above 24 px could go
  the same way, at which point the 88 px and 260 px faces may want the glyph
  cache this record deliberately omits.
