# Design & development guide

Single source of truth for **what** `duckdb-healthkit-export` is, **how** it is built, and **what must not change** without an explicit decision.

- Product overview & install: [README.md](../README.md), [QUICKSTART.md](QUICKSTART.md)
- Forward plan (v0.2+, DuckDB 2.0 packaging): [ROADMAP.md](ROADMAP.md)
- Data model / ERD (export → scanner → local DuckDB): [docs/ERD.md](ERD.md)
- Shipped versions: [RELEASE_NOTES.md](RELEASE_NOTES.md)

Decisions in this file override older chat notes. Last updated: 2026-10-27.

---

## Project in one paragraph

DuckDB **scanner** extension: stream HealthKit-format `export.zip` / `export.xml` into SQL tables.
Language: **C**. ABI: DuckDB **stable C API** only (no `duckdb.hpp`, no unstable C++ extension template).
Community `INSTALL healthkit_export FROM community` ships on DuckDB **2.0** (and prior 1.5.x CDN). Local **unsigned** `LOAD` remains for developers.
Python is for fixtures, tests, and optional add-ons only. Rust/Mojo out of scope.

```text
export.zip | export.xml | export-dir
        → stream parse
        → typed DataChunks
        → SQL / COPY Parquet
```

Not a warehouse, MCP server, dashboard, or dbt package.

---

## Locked decisions

| Topic | Decision |
|---|---|
| GitHub | Public `mjboothaus/duckdb-healthkit-export` (not the old personal C++ stub) |
| Licence | Apache-2.0 |
| Language | **C.** Thin C++ only if the stable table-function C API is unusable |
| ABI | Stable C API. No `#include <duckdb.hpp>`, no unstable extension internals |
| DuckDB target | 2.0 path strategically; v0.1 verified load on 1.5.x unsigned C-API binaries |
| Community `INSTALL` | Live for `healthkit_export` (2.0 preferred; 1.5.x CDN remains until EOL) |
| Dev machine first | macOS Apple Silicon (`osx_arm64`) |
| Wasm | Out of v0.1 |
| Parser | Streaming tag scan (no libxml2 DOM). Zip via minimal reader + zlib |
| Python | Fixtures, golden tables, pytest, optional marimo — not inside the extension |
| Privacy | No telemetry, no network I/O, no real Health exports in git/CI |

### Why C (default)

- Output is flat rows, not object graphs
- SAX-style callbacks are C-shaped
- ABI bet: talk to DuckDB only through `duckdb_extension.h`
- If bind/init/emit in C becomes unmaintainable, switch **only** `healthkit_export_tf.c` to a stable C++ wrapper; leave `parse_health.c` / `zip_source.c` as C

### Hard rules (do not break casually)

1. Parser and zip code must not `#include` DuckDB headers. Only table-function / entrypoint files may.
2. Do not set `USE_UNSTABLE_C_API=1` without an explicit decision.
3. Do not switch to `duckdb/extension-template` (unstable C++ internals) as the default.
4. Do not add Wasm, MCP, dbt, or telemetry in-tree without scope change. (GPX routes + privacy-safe ECG scanners are in-tree.)
5. Do not commit a real Health export.
6. Prefer the smallest diff that advances the current goal.

Personal stub `Mjboothaus/duckdb-ext-apple-health` is a C++ hello-world — **do not copy it.**

---

## Architecture

```text
path
  ├─ zip  → member **/export.xml   (zip_source.c + zlib)
  │         + workout-routes/*.gpx + electrocardiograms/*.csv
  ├─ dir  → export.xml (+ sidecars)
  └─ xml  → file
        │
        ▼
 parse_health.c / parse_gpx.c / parse_ecg.c   (no DuckDB headers)
        │
        ▼
 healthkit_export_tf.c + healthkit_export_extra_tf.c  (C API table functions)
```

### Semantic rules

1. **Memory:** implementation currently buffers in bind (known limit). **Design intent:** peak memory ≈ one XML element + one output chunk (~2–8k rows) once streaming execute lands ([STREAMING_PLAN.md](STREAMING_PLAN.md)).
2. Unknown attributes/tags are ignored (export DTD drifts by iOS version).
3. **`Correlation` child `Record`s:** `read_healthkit_export` emits **top-level** `Record` only. Nested copies are available via `healthkit_correlation_members`. Differs from `healthkit-to-sqlite`, which counts every nested `Record` — see tests.
4. **ECG privacy:** CSV **Name** and **Date of Birth** headers are never exposed as columns or values.
5. `ClinicalRecord` / CDA / audiogram / vision: not first-class TFs yet (P5).

### SQL surface (summary)

Core: `read_healthkit_export` (optional `types`/`start`/`end`), `healthkit_workouts`, `healthkit_activity_summaries`, routes + route points.

Additional: workout events/statistics; export_info / me / zip_inventory; correlations + members; metadata; heartbeat points; ECG recordings/samples.

Full matrix: [README.md](../README.md), [ERD.md](ERD.md), [ADDITIONAL_API_PLAN.md](ADDITIONAL_API_PLAN.md).

**Date parse:** `yyyy-MM-dd HH:mm:ss Z` (`+1100`, `-0800`, `+0530`) → `TIMESTAMPTZ`.

**Value split:** if attribute parses as double → `value` set, `value_text` empty; else `value` null, `value_text` = raw.

### Layout (current)

```text
README.md  CHANGELOG.md  CONTRIBUTING.md  SECURITY.md  LICENSE
docs/          # DESIGN, ERD, ROADMAP, QUICKSTART, RELEASE_NOTES, …
justfile  Makefile  CMakeLists.txt
src/
  parse_health.{c,h}  parse_gpx.{c,h}  parse_ecg.{c,h}  zip_source.{c,h}
  healthkit_export_tf.c  healthkit_export_extra_tf.c  healthkit_export_extension.c
  parse_health_main.c   # CLI
scripts/make_fixture.py
test/data/  test/sql/
tests/                  # pytest vs golden + healthkit-to-sqlite
notebooks/explore_export.py
duckdb_capi/  extension-ci-tools/
```

---

## Current state

| Area | Status |
|---|---|
| C-API template vendored as `healthkit_export` | Done |
| Community INSTALL (2.0 + prior 1.5.x CDN) | Done |
| Streaming XML parser + CLI | Done |
| Zip / dir / xml paths + inventory | Done |
| Core + additional table functions (P1–P4) | Done — see README |
| Named record filters `types`/`start`/`end` | Done (bind/collect) |
| Fixture golden + pytest vs `healthkit-to-sqlite` | Done |
| Real-export smoke (multi-million rows) | Done (manual); formal HK full-import optional |
| Streaming execute + true pushdown | Not yet — [STREAMING_PLAN.md](STREAMING_PLAN.md) |

### Gate history (v0.1 build)

Original implementer gates (from the former implementation brief). All completed for preview:

| Gate | Outcome |
|---|---|
| 0 Machine / tools | Pass (justfile `path_exists == "true"`; DuckDB 1.5.x on PATH) |
| 1 Vendor `extension-template-c` | Pass |
| 2 Unsigned load | Pass on 1.5.2 |
| 3 Hardcoded TF spike | Pass (stable C API has table functions) |
| 4 Parser CLI vs golden | Pass (7 records; Correlation nested skipped) |
| 5 Zip/dir source | Pass |
| 6 Wire TF to zip | Pass (`TIMESTAMPTZ`) |
| 7 Workouts + summaries | Pass |
| 8 Docs polish | Pass (v0.1.0-beta docs pack) |

### Mac workflow

```bash
just check-tools
just configure
just debug
just fixture
just demo
just pytest-ext
```

```sql
-- duckdb -unsigned
LOAD 'build/debug/extension/healthkit_export/healthkit_export.duckdb_extension';
FROM read_healthkit_export('test/data/export.zip');
```

---

## Correctness notes

- **Golden fixture:** top-level records include Height + HRV (+ original set); sleep uses `value_text`; non-ASCII `Café Run Club`; BP also nested under Correlation (must not double-count in `read_healthkit_export`).
- **vs healthkit-to-sqlite:** compare top-level multisets; expect HK total records = ours + nested Correlation children when those exist.
- **Real exports:** keep outside git; optional `HEALTHKIT_EXPORT_ZIP` for slow pytest.

---

## Blockers & mitigations

| Blocker | Mitigation |
|---|---|
| 2.0 headers / community CI churn | Isolate DuckDB glue in TF file; pin `duckdb_capi`; unsigned until CI supports C-API |
| Multi-GB scan feels slow | Streaming execute + `types`/`start`/`end`; document COPY→Parquet |
| Apple date / schema drift | Tests; ignore unknown attrs |
| Tempted to DOM-parse XML | No — streaming only |
| Tempted to copy personal C++ stub | No |

---

## Out of scope (do not start without a decision)

- Per-type physical tables inside the extension
- Silent Watch + iPhone dedupe
- Live HealthKit sync
- MCP / Streamlit / dbt as core deliverables
- Depending on `webbed`
- Shipping a real export in CI
- Unstable C API / C++ template as default

---

## For implementers (LLM or human)

1. Read this file + [ROADMAP.md](ROADMAP.md) before coding.
2. Prefer smallest diff; do not expand scope.
3. Run `just pytest-ext` after behavioural changes.
4. Do not batch large unrelated refactors without evidence.
5. After each substantial change: commands run, key output, files touched.
