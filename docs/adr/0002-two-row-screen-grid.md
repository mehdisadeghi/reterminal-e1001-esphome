# 0002 — Status bar and pages are two rows of a grid

**Status:** Accepted

## Context

Making the status bar's height configurable exposed how its geometry was
spread around: the bar drew itself at a literal `y=446` with a literal height
of `34`, every element inside it baselined at `463`, the analog page computed
its corridor from `bar_on_[page] ? 446 : 474`, and the battery hairline used
`442 : 476`. Nothing derived from anything else.

A taller bar would therefore have been drawn *over* page content rather than
displacing it — the explicit failure the request called out.

Two approaches were considered:

1. **A global vertical transform** — author pages against the old height and
   scale them into whatever is left. ESPHome's `Display` offers no such
   transform, and a per-coordinate mapping would have touched every draw call
   in the file.
2. **Pages derive their own extent** from a shared bottom. Larger conceptual
   change, far smaller diff, and it fits how `draw_analog_clocks` already
   worked — it solved its dial radius from available space rather than fixed
   coordinates.

## Decision

Two rows. The bar row is `bar_h()` tall at `bar_top()`; the content row is
everything above it, ending at `content_bottom(page)` — which is the bar's
top edge, or a bezel margin on pages that hide the bar.

Every page-bottom constant now derives from `content_bottom()`. No page names
446 or 474 any more. The bar cannot grow into a page because the page's floor
*is* the bar's top edge.

Crucially, **every formula reproduces the constant it replaced at the default
size**, so the change is provably invisible until someone changes the bar.
The world clock, graph and Khayyam pages clamp to their tuned depth, so
hiding the bar cannot stretch them either — only a taller bar compresses.
This was verified arithmetically rather than by reading: 17 reworked formulas
against their previous values, in both the bar-shown and bar-hidden cases.

## Consequences

Changing the bar's height is now a one-line change with no page edits, and a
new page gets correct behaviour by asking `content_bottom()` for its floor.

Pages that hard-code interior coordinates (numbers, calendar, noqte) were
left alone because their ink already ends above the tightest content row.
They are not grid-aware, and a much taller bar would eventually reach them —
the grid bounds the *floor*, it does not rescale page interiors.

The compression is proportional, not typographic: at a large bar the Khayyam
lines tighten but the type does not shrink. There is a floor beyond which
that stops looking right.

## Revisit if

- A page's interior needs to scale, not just its bottom — that needs real
  per-page layout, not a shared floor.
- More than two rows are needed (a header, say), which would make the ad-hoc
  `content_bottom()` shape insufficient.
