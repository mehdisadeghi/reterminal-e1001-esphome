# 0003 — Status-bar sizes are a ladder of compiled faces

**Status:** Superseded by [0006](0006-on-device-rasterization.md)

Kept because "why not just compile the sizes?" is the obvious question, and
this is the measured answer.

## Context

The status bar's type size became a user setting (accessibility: a larger
bar for legibility). ESPHome's `font:` component rasterizes TTF glyphs at
**build time** via FreeType and emits bitmaps into flash; the `.ttf` never
reaches the device and `Font::print()` blits fixed bitmaps with no transform.

So a runtime size could only be chosen from sizes compiled in advance.

## Decision

A ladder of compiled faces at 20 / 25 / 30 / 35 / 40 px, one pair per rung
(Inter for English, Vazirmatn for Persian), selected by index at runtime. The
HA control was a `number` with `min 20, max 40, step 5` — min/max/step being
the only way ESPHome numbers express "these values exist", and stricter than
snapping a free-form entry behind the user's back.

## Consequences

It worked, and it was coarse: 20 → 25 is a 25% jump, and no size between them
was reachable.

The measured cost, from the build map file:

| | bitmaps | glyph table | total |
|---|---|---|---|
| 25 px pair | 6.6 KB | 10.0 KB | 16.5 KB |
| 30 px pair | 10.2 KB | 10.0 KB | 20.2 KB |
| 35 px pair | 14.8 KB | 10.0 KB | 24.7 KB |
| 40 px pair | 20.1 KB | 10.0 KB | 30.0 KB |
| **four rungs** | | | **91.4 KB** |

The structural problem is the middle column: **~10 KB per rung is per-glyph
metadata independent of point size**. Finer steps pay that toll per rung
before a single pixel of bitmap, which is what made 1 px granularity
(~441 KB) unaffordable rather than merely expensive.

A second measurement decided the matter: faces ≤24 px — where hinting earns
its keep — were only 54 KB of the 462 KB the project spent on fonts. The
408 KB was in large display sizes, exactly where hinting is least missed.

## Superseded

[0006](0006-on-device-rasterization.md) carries subset outlines and
rasterizes on demand: any size, and 65 KB *less* flash than this ladder.
