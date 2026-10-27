# Roadmap

Living product direction for [duckdb-healthkit-export](https://github.com/Mjboothaus/duckdb-healthkit-export).
Operational checklist: [RELEASE_PLAN.md](RELEASE_PLAN.md). Audience: [PERSONA.md](PERSONA.md).
Versioning: [VERSIONING.md](VERSIONING.md).

## Product layers

| Layer | What | Ships when |
|-------|------|------------|
| **A — Core extension** | C + stable C API: zip/XML → typed table functions | **v0.1.0** (first public release) |
| **B — Enriched local store** | Optional Python: `build-db`, Parquet, `HealthkitStore`, views | usable now; package polish with / after v0.1 |
| **C — Experience add-ons** | Photos ATTACH, journeys YAML, Folium maps, marimo walk-stories | optional; not required for extension release |

Core first. Add-ons must not block the extension binary or community-extension path.

## Cousins (positioning)

- **[webbed](https://github.com/teaguesterling/duckdb_webbed)** — generic XML/HTML in DuckDB ([docs](https://duckdb.org/community_extensions/extensions/webbed.html)). We specialise for HealthKit exports.
- **[healthkit-to-sqlite](https://github.com/dogsheep/healthkit-to-sqlite)** — zip → SQLite batch tool. We keep analysis **in DuckDB** with SQL table functions.

## Performance stance

Raw XML scan is intentionally the **slow path**; Parquet / local DuckDB is the **fast path** (see README).

**v0.1 honesty:** bind-time parse + buffering can use large RAM on multi‑GB exports; full GPS ingest is minutes-scale. Documented so users materialise early.

**Post-v0.1 performance work (priority order):**

1. Stream rows in **execute** (chunked emission; lower peak RAM) — see [STREAMING_PLAN.md](STREAMING_PLAN.md)
2. Streaming inflate from zip (avoid full XML extract when possible)
3. True filter **pushdown** during streaming (named `types`/`start`/`end` already work at bind/collect)
4. String interning for repeated type/source strings
5. Incremental `build-db` (skip GPX already present by path)
6. Optional progress / estimated row counts for long scans

## Shipped on main (baseline)

- Core table functions: records, workouts, activity summaries, workout routes, route GPX points
- Fixture zip + `just` / `uv` developer path; CI smoke
- Python: local DuckDB builder, maps (selected journeys), Photos helpers, marimo apps
- Docs: architecture, privacy, persona, release plan, versioning; repo under **Mjboothaus**

## Shipped (v0.1.0 / v0.1.1 / v0.3.x)

- [x] Phase 1 core freeze — tag **v0.1.0**
- [x] Community extension — **v0.1.1+**; branding **`healthkit_export`** ([#2685](https://github.com/duckdb/community-extensions/pull/2685))
- [x] Linux/Windows community/CI — **v0.3.0**; DuckDB **2.0.0** rebuild ([#3397](https://github.com/duckdb/community-extensions/pull/3397))
- [x] Python tree as optional supplementary package (`healthkit-store`)

## Shipped additional SQL API (P1–P4 on feature branch → main via PR)

See [ADDITIONAL_API_PLAN.md](ADDITIONAL_API_PLAN.md) and README SQL API:

- [x] Workout events + statistics
- [x] `export_info` / `me` / `zip_inventory`
- [x] Named record filters `types` / `start` / `end`
- [x] Correlations + correlation members + metadata
- [x] Heartbeat points (HRV IBPM) + ECG recordings/samples (Name/DOB omitted)

## Near-term

- [ ] Community `description.yml` `repo.ref` bump after additional-API merge + tag
- [ ] **Marimo** `map_walks` / `walk_stories`: smoke-test or mark experimental in docs (library Folium path is source of truth)
- [ ] Keep Python package optional; no PyPI required yet
- [ ] Streaming execute (performance list above)

## After current API surface

- Streaming + true filter pushdown during execute
- Workout route ↔ workout join helpers (SQL examples / optional view)
- Clinical / CDA / audiogram / vision (P5) if fixtures + demand
- Optional: publish supplementary Python package


## Platforms

- [x] macOS (`osx_arm64`; `osx_amd64` on 1.5.x CDN only — dropped for DuckDB 2.0 community)
- [x] Linux / Windows native — v0.3.0 (export.zip is OS-agnostic)
- [ ] **Wasm / browser** — deferred. Real Health exports are large; the scanner still buffers in bind (high RAM). Revisit after streaming execute. Demo/fixture-only Wasm may come first.

## Explicit non-goals (for now)

- Replacing the Health app or clinical decision support
- Shipping personal exports, Photos libraries, or map HTML with PII in git
- Supporting every HealthKit type edge-case in v0.1
- Requiring Python to use the extension

## Open product questions

- Default recommended path for newcomers: SQL-only vs `build-db` first?
- PyPI name and scope for the supplementary package (helpers only vs maps too)?
- How aggressive to be on breaking SQL column names before 1.0 (see VERSIONING)?
