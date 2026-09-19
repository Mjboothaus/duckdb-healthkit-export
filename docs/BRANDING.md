# Branding and trademarks

Last updated: 2026-09-19.

## Product names (ours)

| Name | Use for |
|------|---------|
| **duckdb-healthkit-export** | GitHub repository |
| **healthkit_export** | DuckDB extension (`INSTALL` / `LOAD` / binary) |
| **healthkit-store** | Optional Python package (`import healthkit_store`) |
| **healthkit_store.duckdb** | Default local DB path under `output/` |

Do **not** name the extension, package, or repo with `apple_health` or “Apple Health”.

## What we may say (factual)

These are OK in docs and UI copy when accurate:

- **Health app** — the iOS/watchOS app that produces the export
- **HealthKit** — Apple’s framework / the export format family (`export.zip`, `export.xml`)
- **HealthKit-format export** / **Health app export** — the zip/xml this scanner reads
- Path segments that appear in real exports, e.g. `apple_health_export/` inside a zip (that is the on-disk layout Apple emits; code must recognise it)
- XML/field identifiers from the export schema (e.g. `appleMoveTime`, HealthKit type strings)
- Third-party project names such as **healthkit-to-sqlite**

## What to avoid

- Implying this project is made by, endorsed by, or affiliated with Apple
- Using Apple logos, Health app icons, or marketing artwork
- Product-style titles such as “Apple Health for DuckDB” or “Official Apple Health extension”
- Leading with “Apple Health” as if it were *our* product name (prefer **healthkit_export**)

## Required disclaimer

User-facing docs (README at minimum; community `extended_description` when publishing) must include:

> Not affiliated with, endorsed by, or sponsored by Apple Inc. Apple, Apple Health, and HealthKit are trademarks of Apple Inc.

## Historical note

A short-lived community listing used the id `apple_health` (PR duckdb/community-extensions#2653). That was replaced by `healthkit_export` (PR #2685). Migration docs may still mention `apple_health` as the **old id only**.

## Checklist for new docs / UI

- [ ] Extension referred to as `healthkit_export`
- [ ] Export described as Health app / HealthKit-format export
- [ ] Disclaimer present on primary landing doc
- [ ] No Apple artwork
- [ ] Real export folder name `apple_health_export` only appears as a technical path fact if needed
