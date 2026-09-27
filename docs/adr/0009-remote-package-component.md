# 0009 — The repository is a remote package with a component

**Status:** Accepted

Supersedes [0008](0008-one-config-adopted-per-unit.md) in how units are
built; the MAC-derived identity and runtime helper ids stand.

## Context

Adopting a unit in the ESPHome Builder writes a config that pulls this
repository's main YAML as a remote package; the Builder does not honour
`import_full_config`. A remote package resolves `!include` inside the
cloned repository, but fonts, `esphome: includes:` and the partition file
resolve against the config directory on the HA host, and the version
stamp and dial photos were generated files outside git. The workaround —
copying the tree to the HA host with `make deploy` — made every unit
depend on a manual sync.

## Decision

Everything a build needs comes from the repository or from the unit's
own config:

- The C++ is an external component (`components/reterminal`), fetched
  from GitHub like the package. Its headers live in `src/` and are copied
  into the build and included once from `main.cpp`, as `includes:` did:
  headers beside the component's `__init__.py` would be included into
  every translation unit, and these define globals.
- Fonts are fetched by URL from the repository.
- The partition table is declared inline (the 24 MB `hist` partition);
  ESPHome lays out the rest.
- The component stamps the version from `git describe` of its own source,
  replacing the placeholder in the YAML.
- Dial photos are listed per installation (`reterminal: dial_images:`)
  as URLs or paths; the component fetches and bakes them at build time.

`component_source` and `font_dir` are substitutions: GitHub by default,
this tree for local builds (the Makefile overrides both).

## Consequences

A new owner flashes a unit, adopts it, and updates it from the Builder;
no ssh, no copy step. The repository carries no photos.

A build needs internet access to GitHub. Builder builds are stamped with
the commit hash (ESPHome clones shallowly, without tags).

Replacing the core's project-version define from the component reaches
into ESPHome's code generation; an ESPHome change there breaks the stamp,
not the build.

The generated partition table gives 3.75 MB app slots. Units flashed with
the earlier hand-written table keep it (OTA cannot rewrite a table) and
its 3 MB slots, which every build must still fit.

## Revisit if

- ESPHome lets a remote package carry files for `includes:`, fonts and
  partitions — the component would shrink to the photos and the stamp.
- Photos need to differ per unit beyond the SD override.
