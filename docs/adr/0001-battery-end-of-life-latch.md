# 0001 — Battery end of life is a latched takeover, not a page

**Status:** Accepted

## Context

An ePaper panel holds its last frame with no power at all. When the pack runs
out, whatever was on screen stays there indefinitely — so a dead device shows
stale clocks and climate numbers, indistinguishable from a device that is
merely frozen. The frame a dead device is left holding should explain itself.

Two constraints shaped the answer:

- A panel refresh at low voltage can leave a half-drawn or ghosted frame. The
  message has to be painted while there is still headroom for a *clean*
  refresh, not at the moment the battery dies.
- `battery_level` is a `calibrate_linear` curve over a single ADC read, and
  the curve is an unvalidated guess (still an open TODO). Pack voltage also
  rebounds once the radio goes quiet, so one sample can dip below any
  threshold during a transmit.

The obvious framing — "a page the device navigates to" — was considered and
rejected. Pages participate in navigation, Show switches, auto-cycle and the
start page, none of which should apply to an emergency screen; making it a
page means fighting all of them to keep the user on it.

## Decision

A latch (`battery_critical`) in RTC memory, checked in the display dispatch
*before* page selection, drawing over whatever page is current.

- Thresholds are on **raw voltage**, not the percentage curve: warn tiers at
  3.58 / 3.49 / 3.41 V thicken the battery hairline, and 3.35 V latches. That
  leaves headroom above the 3.27 V floor for one full refresh.
- Latching requires **two consecutive wakes** below the line, so a transmit
  dip cannot trigger it.
- Once painted, `charge_hold()` suppresses further redraws. The panel keeps
  the frame unpowered; repainting only risks a bad one.
- The device does **not** sleep forever. It wakes every 30 minutes with the
  radio down, purely to notice a recharge; ≥3.70 V clears the latch and
  normal operation resumes.
- The same screen is also page 10 with its Show switch off by default, so it
  can be inspected without draining a pack. That is a *viewing* affordance,
  not the mechanism.

## Consequences

The last frame explains the device instead of lying about the time. The cost
is that the page underneath is lost — deliberate: the data lives in Home
Assistant, the panel is a viewport, and the user has had days of a thickening
gauge as warning. The status bar still renders on the charge screen, so the
date and battery percentage survive alongside it.

Fixed thresholds mean a pack with a different discharge curve latches at the
wrong moment. Acceptable while the calibration itself is unvalidated.

## Revisit if

- Real discharge measurements land (open TODO) and show the tiers are wrong.
- A different cell or charger changes the recovery voltage, making 3.70 V
  either unreachable or trivially crossed.
