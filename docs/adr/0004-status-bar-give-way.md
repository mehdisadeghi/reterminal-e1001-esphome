# 0004 — The status bar drops items rather than clamping the font

**Status:** Accepted

## Context

The bar builds a start-side group (weekday, then the enabled calendars) and
an end-side group (battery, awake bulb, SD icon, RTC warning, climate), each
growing inward from its margin. At the default type size they fit. At a
larger one they collided in the middle, and long strings ran off the panel.

The options were to cap the font size at what always fits (hard-coding a
limit, and a conservative one, since the worst case is Persian with three
calendars enabled), or to let the row adapt.

## Decision

Measure, then let the lowest-value items give way.

Both groups are measured at the active face via `get_text_bounds` before
anything is drawn. While the row cannot hold them, items are dropped in
order: **battery percentage → climate → RTC → SD → bulb → lunar Hijri →
solar Hijri**. Weekday and the Gregorian date always survive.

Nothing is sized by hand, so the cutoff follows the actual glyphs — it
differs between English and Persian without either being special-cased.

The battery percentage leads that list **only when the hairline gauge is
on**, since the gauge already carries the same reading down the edge. With
the gauge off, the percentage is the sole battery indication and keeps its
old priority, with climate giving way first.

## Consequences

Any font size is selectable and degrades legibly instead of overprinting or
running off the panel. Dropping an item is also honest feedback that the
chosen size is too big.

At the **default** size a maximal configuration (both optional calendars,
plus RTC warning, SD card and awake bulb, plus climate) now drops one item
where it previously overlapped. That is the fix working, but it is a visible
difference at a size nobody changed.

The give-way order encodes a value judgement — that an icon is worth more
than a calendar date — which is arguable and was in fact revised once, when
the battery percentage was demoted below climate.

## Revisit if

- The order is wrong in practice; it is a single array and cheap to reorder.
- A future item has no sensible rank, suggesting the flat priority list
  should become per-item weights.
