# Changelog

## [0.3.1] — 2026-09-19

### Fixed
- Windows (and other vcpkg CI) builds: add `vcpkg.json` with **zlib** so CMake `find_package(ZLIB)` succeeds.

## [0.3.0] — 2026-09-19

### Added
- Native **Linux** and **Windows** community/CI builds (export.zip is OS-agnostic).

### Changed
- `excluded_platforms` / CI `exclude_archs`: only **Wasm** variants remain excluded.

### Notes
- Wasm / browser support stays on the roadmap (large exports + bind-time memory).

## [0.2.0] — 2026-09-12

### Changed (breaking)
- Extension id and binary renamed **`apple_health` → `healthkit_export`**.
- Table functions renamed (no aliases):
  - `read_healthkit_export`
  - `healthkit_workouts`
  - `healthkit_activity_summaries`
  - `healthkit_workout_routes`
  - `healthkit_workout_route_points`
- Community listing will move to `extensions/healthkit_export/` (replace short-lived `apple_health` publish).

### Notes
- Repo remains `Mjboothaus/duckdb-healthkit-export`; optional local DB path remains `output/healthkit_store.duckdb`.
- Python package remains `healthkit-store`.

## [0.1.1] — 2026-09-10

### Added
- DuckDB **community extension** on **macOS** (`INSTALL healthkit_export FROM community`).
- Listing: [duckdb/community-extensions#2653](https://github.com/duckdb/community-extensions/pull/2653) (merged).
- `healthkit_store.connect_with_healthkit_export` / `load_healthkit_export` (community first, local unsigned fallback).
- Docs + `explore_export` notebook updated for community install.

### Fixed
- C extension ABI stamp: `TARGET_DUCKDB_VERSION=v1.2.0` (was incorrectly v1.5.6, which community hosts reject).

### Platforms
- Community CDN: `osx_arm64`, `osx_amd64` for DuckDB **v1.5.5**.

## [0.1.0] — 2026-09-06

### Added
- Core extension release tag path: `VERSION` = `0.1.0`, git tag `v0.1.0` for extension metadata.
- `just pytest-ext` / `pytest-ext-real` recipes for extension pytest.
- Docs: [CREATE_COMM_EXT.md](docs/CREATE_COMM_EXT.md) (community INSTALL migration).
- Companion package name **healthkit-store** (optional; not required for extension use).

### Extension (Layer A)
- Table functions: `read_healthkit_export`, `healthkit_workouts`, `healthkit_activity_summaries`,
  `healthkit_workout_routes`, `healthkit_workout_route_points`.
- SQLLogic + fixture golden + healthkit-to-sqlite top-level compare green on freeze.

### Changed
- README / RELEASE_NOTES promote **v0.1.0** core vs optional add-ons.
- Honest performance limits retained (bind buffer; materialise for fast path).

### Not included in core binary
- Python maps/Photos/journeys/marimo (same repo, optional).
- Streaming execute, named scan filters (community install arrived in **0.1.1**).

All notable changes to this project are documented here.
Format inspired by [Keep a Changelog](https://keepachangelog.com/).
Versioning: early **0.x** beta tags; breaking changes allowed until 1.0.

## [Unreleased]

### Added
- `healthkit_workout_events(path)` and `healthkit_workout_statistics(path)` (workout children).
- `healthkit_export_info(path)`, `healthkit_me(path)`, `healthkit_zip_inventory(path)`.
- `healthkit_correlations(path)`, `healthkit_correlation_members(path)`, `healthkit_metadata(path)`.
- `healthkit_heartbeat_points(path)`, `healthkit_ecg_recordings(path)`, `healthkit_ecg_samples(path)` (ECG **omits Name / Date of Birth** headers).
- Named filters on `read_healthkit_export`: `types`, `start`, `end` (VARCHAR).
- Design note: [docs/ADDITIONAL_API_PLAN.md](docs/ADDITIONAL_API_PLAN.md).

### Changed
- Target **DuckDB 2.0.0 LTS** as the CI/host build (`extension-ci-tools` → `v2.0-cyanoptera`, `duckdb_version: v2.0.0`).
- Keep **stable C Extension API** (`C_STRUCT`, `TARGET_DUCKDB_VERSION=v1.2.0`, V1 entrypoint). C API v2 is experimental and not required.
- CI `exclude_archs` adds **`osx_amd64`** (community extensions no longer built for Intel macOS from DuckDB 2.0).

### Notes
- Community `healthkit_export` rebuilt for DuckDB 2.0.0 ([community-extensions#3397](https://github.com/duckdb/community-extensions/pull/3397)).
- DuckDB 2.0 uses storage version 69 by default; files written by 2.0 are not readable by DuckDB &lt; 2.0.

### Planned

- Streaming table-function execute (bounded memory)
- Correlations / metadata / ECG table functions (see ADDITIONAL_API_PLAN)

See [ROADMAP.md](docs/ROADMAP.md).

## [0.1.0-beta] — 2026-08-29

First public **developer beta**.

### Added

- C extension on DuckDB stable C API (`healthkit_export`)
- `read_healthkit_export(path)` with TIMESTAMPTZ dates and value / value_text split
- `healthkit_workouts(path)` and `healthkit_activity_summaries(path)`
- Zip / directory / XML path support
- Synthetic fixtures, golden CSV, pytest vs `healthkit-to-sqlite`
- `just` recipes: build, demo, pytest-ext
- Optional marimo explorer notebook
- DESIGN, ROADMAP, RELEASE_NOTES, CONTRIBUTING, SECURITY

### Known limitations

- Parse buffers in bind (high RAM on multi-GB exports)
- Unsigned local load only
- Named scan filters not implemented
- Nested Correlation `Record` children intentionally skipped

Full notes: [RELEASE_NOTES.md](docs/RELEASE_NOTES.md).

[Unreleased]: https://github.com/Mjboothaus/duckdb-healthkit-export/compare/v0.3.1...HEAD
[0.3.1]: https://github.com/Mjboothaus/duckdb-healthkit-export/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/Mjboothaus/duckdb-healthkit-export/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/Mjboothaus/duckdb-healthkit-export/compare/v0.1.1...v0.2.0
[0.1.1]: https://github.com/Mjboothaus/duckdb-healthkit-export/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/Mjboothaus/duckdb-healthkit-export/compare/v0.1.0-beta...v0.1.0
[0.1.0-beta]: https://github.com/Mjboothaus/duckdb-healthkit-export/releases/tag/v0.1.0-beta
