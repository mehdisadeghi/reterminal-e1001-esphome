# 0007 — The battery gauge is a column outside the grid

**Status:** Accepted

## Context

The optional battery hairline runs down the panel edge, filled from the
bottom in proportion to charge. When [0002](0002-two-row-screen-grid.md)
made page bottoms derive from the status bar, the gauge was migrated along
with them — it ended at the bar's top edge on pages showing the bar, and at
the panel edge on pages hiding it.

That inherited an existing bug and made it worse. The gauge's *span* was its
100% mark, so the same charge drew a different length depending on whether
the page showed the status bar, and any change to the bar's height silently
rescaled it. A gauge whose full-scale moves is not a gauge.

## Decision

The gauge is not part of the row grid. It is its own column spanning the full
panel height, inset only for the bezel. Its scale is fixed, so neither the
bar's presence nor its height affects the level it reads.

Where it crosses the inverted status bar it is drawn in the bar's foreground
colour, so the column stays continuous instead of disappearing into the fill.

## Consequences

The gauge is comparable across pages and across configuration changes, which
is the only property that makes it worth having.

It overlaps the status bar's rectangle by design, so it must be drawn after
the bar and know the bar's extent for the colour flip. That is a small,
explicit coupling in exchange for a stable scale.

This is the one place that deliberately ignores the grid, which is why it is
recorded rather than left to be "tidied up" later into consistency with
[0002](0002-two-row-screen-grid.md).

## Revisit if

- Another always-visible edge element appears, suggesting a proper "outside
  the grid" concept rather than one special case.
