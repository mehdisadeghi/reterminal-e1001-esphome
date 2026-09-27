# 0008 — One config, adopted per unit in the ESPHome Builder

**Status:** Accepted

## Context

A unit's identity — hostname, display name, and the entity ids of its
three HA helpers — was a compile-time substitution. Every unit therefore
needed its own YAML, and those YAMLs recorded one installation's fleet in
a repository meant to be generic.

## Decision

The repository holds one config. A fresh unit appends its MAC to its name
(`name_add_mac_suffix`) and derives its helper entity ids from that name
at runtime; the helpers are subscribed from C++ before HA can connect,
which is when HA reads the subscription list.

The firmware is adoptable (`dashboard_import`). Adopting a unit in the
ESPHome Builder writes the unit's YAML on the HA host: a copy of the main
config with the name pinned (`import_full_config`). The main config is
kept thin — identity, packages, the import URL — so the copies do not go
stale; the code lives in the shared tree, which `make deploy` puts beside
them, generated files included.

Updates go through the Builder (Install, Update All) as for any ESPHome
device.

## Consequences

Per-unit files exist only on the HA host, where the Builder manages
them; nothing about a unit enters the repository. The Builder shows each
unit's state and logs, and edits its secrets.

A remote package (the Builder's default adoption) would not work: fonts,
C++ includes and the partition table resolve against the config
directory, and the version stamp and dial photos are generated, not
committed. The full-config copy keeps all of that local.

Adoption fetches the main config from the repository named in
`dashboard_import`; a fork changes that URL.

OTA pushes reach only awake units, so an update means switching on the
unit's Keep Awake helper first.

A renamed unit keeps its settings (preferences hash the entity, not the
device name) but gets new helper ids.

## Revisit if

- ESPHome resolves relative paths inside remote packages — adoption
  could then use the default remote package and drop `make deploy`.
- Keeping units awake for updates becomes a burden — ESPHome's
  `update: http_request` lets units pull a published build on wake.
