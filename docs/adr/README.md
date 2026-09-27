# Architecture Decision Records

One file per decision worth defending later. Each records what was true when
the choice was made, what was chosen, what it costs, and — the part that ages
best — **what would make us revisit it**.

A record is never edited to reflect a change of mind. It gets a `Superseded
by` status and a new record takes over, because the reasoning behind a
replaced decision is exactly what stops someone re-proposing it.

Feature-scale design that outgrows one decision lives in its own document:
see [`sd-config-design.md`](../../sd-config-design.md).

| # | Decision | Status |
|---|----------|--------|
| [0001](0001-battery-end-of-life-latch.md) | Battery end of life is a latched takeover, not a page | Accepted |
| [0002](0002-two-row-screen-grid.md) | Status bar and pages are two rows of a grid | Accepted |
| [0003](0003-compiled-font-ladder.md) | Status-bar sizes are a ladder of compiled faces | Superseded by 0006 |
| [0004](0004-status-bar-give-way.md) | The status bar drops items rather than clamping the font | Accepted |
| [0005](0005-debug-page-as-session.md) | The debug page is a bounded diagnostic session | Accepted |
| [0006](0006-on-device-rasterization.md) | Status-bar type is rasterized on the device | Accepted |
| [0007](0007-battery-gauge-outside-grid.md) | The battery gauge is a column outside the grid | Accepted |
| [0008](0008-one-config-adopted-per-unit.md) | One config, adopted per unit in the ESPHome Builder | Superseded by 0009 |
| [0009](0009-remote-package-component.md) | The repository is a remote package with a component | Accepted |
