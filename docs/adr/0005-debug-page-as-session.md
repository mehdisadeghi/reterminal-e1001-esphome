# 0005 — The debug page is a bounded diagnostic session

**Status:** Accepted

## Context

The debug page (double green press) reports Wi-Fi state, RSSI, IP, heap,
uptime and config. It was a plain RAM `bool` overlay, which meant its most
useful rows were the least trustworthy: the device slept moments after the
page appeared, so the readout showed whatever a single wake happened to
catch, and a struck-out or radio-off device reported "gave up" rather than
the network state being investigated.

## Decision

Debug is a session with a deadline, not a screen.

- `debug_until` is a `millis()` deadline rather than a flag. It needs no RTC
  memory precisely *because* the session never sleeps.
- Entering it brings the radio up and clears `wifi_fail_wakes`, so a
  struck-out device can actually connect. It sets `radio_on` directly and
  leaves the persisted Radio switch untouched — the session borrows the
  radio, it does not rewrite configuration. Leaving restores the switch's
  state.
- Sleep is held off, so the live rows keep updating; the existing
  `redraw_due()` path repaints on the normal refresh interval.
- It expires after an hour and drops back to the page underneath. The title
  shows the remaining minutes, so the deadline is visible rather than a
  surprise.

## Consequences

An hour awake with Wi-Fi up is the most expensive thing this device does —
that is the point of the feature, and the expiry is what keeps it bounded.

It interacts with [0001](0001-battery-end-of-life-latch.md): holding an
already-empty pack awake for an hour would finish it. `debug_hold()` skips
the **sleep hold** while `battery_critical` is latched, but still shows the
page — that state is exactly when the diagnostics are worth reading.

A green press during a session still uses the radio, deliberately: it is an
explicit user action and a deliberate escape hatch.

## Revisit if

- An hour proves wrong in practice (it is one constant).
- The session needs to survive deep sleep, which would require moving the
  deadline into RTC memory and reasoning about `millis()` across wakes.
