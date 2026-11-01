# Quick start

**Community install** on DuckDB **2.0+** (preferred) or **1.5.5+** (prior CDN), or local unsigned build for developers.

## 1. Export from the Health app

On iPhone:

1. Health → profile → **Export All Health Data**
2. Copy the zip to your computer (AirDrop, Files, etc.)
3. Optional: unzip — zip, directory, or `export.xml` all work

Multi-GB zips are normal. **Do not commit a real export to git.**

## 2. Install (community — preferred)

Needs a native DuckDB CLI with a published community artifact (**2.0.0** preferred; **1.5.5+** while older CDN builds remain). Platforms: **macOS arm64, Linux, Windows** (`osx_amd64` dropped for DuckDB 2.0 community).

```sql
INSTALL healthkit_export FROM community;
LOAD healthkit_export;
```

If install 404s, upgrade the CLI or use the local build path below.

## 3. Build locally (optional / developers)

Needs Xcode CLT (macOS), CMake, Python 3.12+, and a DuckDB CLI that can load unsigned extensions.

```bash
git clone --recurse-submodules git@github.com:mjboothaus/duckdb-healthkit-export.git
cd duckdb-healthkit-export
brew install cmake ninja ccache just   # macOS example
just check-tools
just bootstrap   # configure (if needed) + debug + fixture
```

```bash
duckdb -unsigned
```

```sql
LOAD 'build/debug/extension/healthkit_export/healthkit_export.duckdb_extension';
```

If `LOAD` cannot find the file: `find build -name '*.duckdb_extension'`.

## 4. Query

### Core scanners

```sql
FROM read_healthkit_export('~/Downloads/export.zip');
FROM healthkit_workouts('export.zip');
FROM healthkit_workout_routes('export.zip');
FROM healthkit_workout_route_points('export.zip');
FROM healthkit_activity_summaries('export.zip');
```

### Workout children

```sql
FROM healthkit_workout_events('export.zip');
FROM healthkit_workout_statistics('export.zip');
```

### Catalogue / sensitive helpers

```sql
FROM healthkit_export_info('export.zip');
FROM healthkit_zip_inventory('export.zip');
FROM healthkit_me('export.zip');  -- sensitive characteristics
```

### Correlations and metadata

```sql
FROM healthkit_correlations('export.zip');
FROM healthkit_correlation_members('export.zip');
FROM healthkit_metadata('export.zip');
```

### Heartbeat + ECG (privacy-aware)

```sql
FROM healthkit_heartbeat_points('export.zip');
FROM healthkit_ecg_recordings('export.zip');
FROM healthkit_ecg_samples('export.zip');
-- optional: FROM healthkit_ecg_samples('export.zip', ecg_path := 'ecg_2026');
```

ECG helpers **never return** CSV **Name** or **Date of Birth** headers.

### Record filters (named parameters)

```sql
FROM read_healthkit_export('export.zip', types := 'HeartRate,StepCount');
FROM read_healthkit_export(
  'export.zip',
  start := '2024-01-01 00:00:00 +1100',
  end := '2024-12-31 23:59:59 +1100'
);
```

`types` is comma-separated `type_short` or full HealthKit type ids. Range filters apply to record `start_date`.

### Soft joins (examples)

```sql
SELECT w.activity_type_short, w.start_date, e.event_type_short, e.event_date
FROM healthkit_workouts('export.zip') w
JOIN healthkit_workout_events('export.zip') e
  ON w.activity_type_short = e.workout_activity_type_short
 AND w.start_date = e.workout_start_date;
```

### Synthetic fixture (no PHI)

```bash
just fixture
just demo
just demo-workouts
just demo-routes
```

```sql
FROM read_healthkit_export('test/data/export.zip');
```

## 5. Materialise once (recommended for large exports)

```sql
COPY (
  SELECT *
  FROM read_healthkit_export('export.zip', types := 'HeartRate')
) TO 'hr.parquet' (FORMAT parquet);

SELECT date_trunc('day', start_date) AS day, avg(value)
FROM 'hr.parquet'
GROUP BY 1
ORDER BY 1;
```

The extension still buffers in bind (high RAM on multi-GB XML). Parquet / `just build-db` is the fast path for repeat analytics.

## 6. Tests

```bash
just debug
just pytest-ext
```

## 7. Optional notebook / local store

```bash
uv sync
just debug   # local build needed for newest table functions
just explore-apis   # P1–P4 API explorer (fixture or real zip)
# HEALTHKIT_EXPORT_ZIP=~/path/to/export.zip just explore-apis
just build-db export_zip=/path/to/export.zip
just explore
just map-walks
```

## What you will see (records)

| Column | Meaning |
|---|---|
| `type` | Full HealthKit id |
| `type_short` | e.g. `HeartRate` |
| `unit` | e.g. `count/min` |
| `value` | Number, or null for categories |
| `value_text` | Category string when `value` is null |
| `start_date` / `end_date` | `TIMESTAMPTZ` |
| `source_name` / `device` | App or Watch string |

Full TF matrix: [README.md](../README.md) · [ERD.md](ERD.md) · [ADDITIONAL_API_PLAN.md](ADDITIONAL_API_PLAN.md).

## Privacy

The extension only reads a path you pass in. No network, no telemetry. Keep real exports off GitHub and out of CI logs. Treat `healthkit_me` and ECG paths as sensitive.

## Not in this tree yet

Wasm, clinical/CDA/audiogram/vision first-class tables, streaming execute, Watch/iPhone dedupe, `ignore_errors`.

## If something fails

| Symptom | Check |
|---|---|
| Community `INSTALL` HTTP 404 | Upgrade DuckDB (**2.0+** preferred); Wasm not published; use native CLI |
| `LOAD` refuses a local file | `duckdb -unsigned`; find `.duckdb_extension` under `build/` |
| Slow / large RAM on big zip | Prefer `types`/`start`/`end`, then `COPY` to Parquet; see [ROADMAP.md](ROADMAP.md) |
| Tests fail | `just debug` then `just pytest-ext` |

More detail: [DESIGN.md](DESIGN.md), [RELEASE_NOTES.md](RELEASE_NOTES.md).
