# Additional API plan — more of `export.zip` in SQL

Last updated: 2026-10-27.  
Status: **design only** (no implementation commitment).  
Related: [DESIGN.md](DESIGN.md) · [ERD.md](ERD.md) · [ROADMAP.md](ROADMAP.md) · [STREAMING_PLAN.md](STREAMING_PLAN.md) · [BRANDING.md](BRANDING.md)

---

## 1. Purpose

Sketch how the **`healthkit_export`** DuckDB extension could expose **more of what Apple’s Health export already contains**, without changing the product identity (scanner over a user-supplied path; no network; no PHI in git).

This is complementary to [STREAMING_PLAN.md](STREAMING_PLAN.md): streaming improves **how** we scan; this plan covers **what else** we return.

---

## 2. Current SQL surface (baseline)

| Function | Source | Status |
|----------|--------|--------|
| `read_healthkit_export(path)` | Top-level `<Record>` in `export.xml` | Shipped |
| `healthkit_workouts(path)` | `<Workout>` summary attrs | Shipped |
| `healthkit_activity_summaries(path)` | `<ActivitySummary>` | Shipped |
| `healthkit_workout_routes(path)` | `<WorkoutRoute>` + `FileReference` | Shipped |
| `healthkit_workout_route_points(path)` | `workout-routes/*.gpx` | Shipped |

**Deliberate skips today** (parser comments / DESIGN): nested Correlation `<Record>` children, `<WorkoutEvent>`, `<MetadataEntry>`, `<Me>`, `<ExportDate>`, clinical/ECG as first-class tables, CDA XML.

**Planned but not API-shaped yet** (README / streaming plan): named parameters `types`, `start`, `end`, `ignore_errors`.

---

## 3. What else is in a real `export.zip`

Inventory from private multi-year exports (structure only; no PHI committed). Counts are order-of-magnitude and vary by user/device history.

### 3.1 Inside `export.xml`

| Element | Role | Approx scale (one large export) | Exposed today? |
|---------|------|----------------------------------|----------------|
| `ExportDate` | When the zip was produced | 1 | No |
| `Me` | Characteristics (DOB, sex, blood type, …) | 1 | No |
| `Record` | Samples / categories | millions | **Yes** (top-level only) |
| `Correlation` | Grouped records (e.g. BP) | few–many | Parent ignored; nested records skipped |
| `Workout` | Workout summary | hundreds–thousands | **Yes** (attrs only) |
| `WorkoutEvent` | Pause/resume/marker under workout | thousands–tens of thousands | No |
| `WorkoutStatistics` | Per-workout aggregates (distance, HR, …) | thousands+ | No |
| `MetadataEntry` | Key/value on Record/Workout/… | hundreds of thousands–millions | No |
| `WorkoutRoute` / `FileReference` | Link to GPX | hundreds–thousands | **Yes** |
| `ActivitySummary` | Daily rings | thousands | **Yes** |
| `InstantaneousBeatsPerMinute` | HR series points (often under a series/workout context) | hundreds of thousands | No |
| `ClinicalRecord` | Clinical document stubs (when present) | 0–n | No |
| `Audiogram` / `VisionPrescription` | Hearing / vision (when present) | 0–n | No |
| `HeartbeatSeries` (and similar series containers) | Structured series wrappers | 0–n | No |

### 3.2 Sidecar files in the zip

| Path pattern | Format | Exposed today? |
|--------------|--------|----------------|
| `workout-routes/*.gpx` | GPX track | **Yes** via route points TF |
| `electrocardiograms/ecg_*.csv` | CSV: header metadata + µV samples | No |
| `export_cda.xml` | Clinical Document Architecture (when present) | No |
| Other clinical attachments | Varies by iOS / locale | No |

ECG CSV shape (conceptual): identity/header lines, classification, sample rate, lead, unit, then one numeric sample per line. High PHI sensitivity in headers.

---

## 4. Design principles

1. **Path-in, rows-out.** Every new capability remains a table function (or named parameter on an existing one) over `path` = zip | dir | xml. No implicit global DB inside the extension.
2. **Stable names.** Prefer `healthkit_*` prefix; avoid Apple product claims in identifiers ([BRANDING.md](BRANDING.md)).
3. **Additive, non-breaking.** New functions and nullable columns only until 1.0. Do not rename shipped columns lightly ([VERSIONING.md](VERSIONING.md)).
4. **One concern per function.** Wide “god tables” are harder to stream and document. Prefer narrow TFs that join on stable keys.
5. **Join keys over opaque rowids.** Prefer natural keys already in the export (`start_date` + type + source, `gpx_path`, workout start + activity) and document softness. Introduce a synthetic `workout_id` only if we can define it stably (e.g. hash of type+start+end+source) and version it.
6. **Streaming-first.** New high-cardinality TFs should target the pull/execute model in [STREAMING_PLAN.md](STREAMING_PLAN.md), not expand bind buffers.
7. **Privacy by default in docs/fixtures.** Synthetic fixtures for every new element; real exports stay local. ECG/CDA docs must stress PHI in headers.
8. **SQL filters vs pushdown.** `WHERE` always works after emit; named parameters are performance/ergonomics, not a second semantics.

---

## 5. Proposed API shapes

Illustrative SQL only — column lists can tighten during implementation.

### 5.1 Cross-cutting: export catalogue + characteristics

Small, one-shot metadata (cheap once streaming/catalogue exists).

```sql
-- One row: when the export was made
FROM healthkit_export_info('export.zip');
-- columns: export_date TIMESTAMPTZ, source_path VARCHAR, has_cda BOOLEAN, …

-- Characteristics from <Me> (highly sensitive)
FROM healthkit_me('export.zip');
-- columns: date_of_birth DATE, biological_sex VARCHAR, blood_type VARCHAR,
--          fitzpatrick_skin_type VARCHAR, …  (nullable; attr-driven)
```

**Open product choice:** ship `healthkit_me` at all, or document “read Me via a one-off XML example” and keep PHI out of the happy path. If shipped, README must treat it as **optional and sensitive**.

### 5.2 Enrich records (parameters, not only new TFs)

```sql
FROM read_healthkit_export(
  'export.zip',
  types := ['HeartRate', 'StepCount'],   -- type_short or full id; list or VARCHAR
  start := TIMESTAMPTZ '2024-01-01',
  end   := TIMESTAMPTZ '2024-12-31',
  ignore_errors := true                  -- skip bad dates vs fail scan
);
```

Optional later columns on the **same** function (nullable, default null / empty):

| Column | Meaning |
|--------|---------|
| `record_id` | Stable hash of type+start+end+source+value for joins to metadata |
| `correlation_type` / `correlation_start` | Set only if we add an **opt-in** nested mode |

**Nested Correlation policy (pick one explicitly before coding):**

| Option | Behaviour | Pros | Cons |
|--------|-----------|------|------|
| **A (default today)** | Top-level `Record` only | Matches healthkit-to-sqlite top-level compare; no double count | BP correlation parent invisible |
| **B** | Add `healthkit_correlations` + `healthkit_correlation_records` | Clear model; joinable | More API surface |
| **C** | Parameter `include_nested_records := false` | One TF | Easy to misuse; breaks counts vs today if default flips |

**Recommendation:** keep **A** as default forever; add **B** if users need correlation structure.

```sql
FROM healthkit_correlations('export.zip');
-- type, type_short, start_date, end_date, source_name, …

FROM healthkit_correlation_members('export.zip');
-- correlation keys + member record columns (or record_id)
```

### 5.3 Workout children (highest practical value next)

Workouts already ship; the export carries rich **children** that analytics users expect.

```sql
FROM healthkit_workout_events('export.zip');
-- activity_type_short, workout_start_date, workout_end_date,
-- event_type, event_type_short, event_date, duration, …

FROM healthkit_workout_statistics('export.zip');
-- activity_type_short, workout_start_date,
-- stat_type, stat_type_short, start_date, end_date,
-- sum_quantity, minimum, maximum, average, unit, …

FROM healthkit_metadata('export.zip');
-- parent_kind ENUM/VARCHAR: 'record' | 'workout' | 'correlation' | …,
-- parent join keys, key, value
```

**Join story (document as soft keys):**

```sql
SELECT w.activity_type_short, w.start_date, e.event_type_short, e.event_date
FROM healthkit_workouts('export.zip') w
JOIN healthkit_workout_events('export.zip') e
  ON w.activity_type_short = e.activity_type_short
 AND w.start_date = e.workout_start_date;
```

If soft keys prove ambiguous, introduce:

```text
workout_uid = hex(sha256(activity_type || start || end || source_name || device))
```

as a computed column on workouts + children (version the hash input list in docs).

### 5.4 Heartbeat / instantaneous BPM series

`InstantaneousBeatsPerMinute` appears at large scale on real exports (often tied to HeartbeatSeries or workout context).

```sql
FROM healthkit_heartbeat_points('export.zip');
-- series/workout join keys, time TIMESTAMPTZ, bpm DOUBLE
```

Alternatively fold into a generic `healthkit_series_points(path, series_type := …)` if more series types appear — only if the XML shapes stay similar.

### 5.5 ECG sidecars

Separate from XML walk: enumerate zip members, parse CSV headers + samples.

```sql
FROM healthkit_ecg_recordings('export.zip');
-- ecg_path, recorded_at, classification, symptoms, software_version,
-- device, sample_rate_hz, lead, unit, n_samples, …

FROM healthkit_ecg_samples('export.zip');
-- ecg_path, sample_index BIGINT, value_uV DOUBLE
```

**Notes:**

- Header fields may duplicate subject identity — document clearly; consider omitting name/DOB columns and keeping device/classification/timing only (product decision).
- Samples are long (tens of thousands per recording): **must** stream; never bind-buffer full ECG.
- Optional parameter: `ecg_path := '/electrocardiograms/ecg_….csv'` to scan one file.

### 5.6 Clinical / CDA / audiogram / vision

Lower priority unless fixtures + clear schemas exist.

```sql
FROM healthkit_clinical_records('export.zip');  -- XML ClinicalRecord attrs + file refs
FROM healthkit_cda_documents('export.zip');    -- enumerate export_cda.xml / attachments
FROM healthkit_audiograms('export.zip');
FROM healthkit_vision_prescriptions('export.zip');
```

Treat as **opt-in milestones** after workout children + ECG metadata. CDA may be “path + hash + length” only at first (avoid pretending full FHIR).

### 5.7 Zip inventory (developer / UX helper)

```sql
FROM healthkit_zip_inventory('export.zip');
-- member_path, uncompressed_size, kind: 'export_xml' | 'gpx' | 'ecg_csv' | 'cda' | 'other'
```

Useful for notebooks and support (“what’s in my zip?”) without parsing XML.

---

## 6. Naming and discoverability

| Pattern | Example |
|---------|---------|
| Existing | `read_healthkit_export`, `healthkit_workouts`, … |
| New entities | `healthkit_<noun_plural>(path)` |
| Avoid | Second “read_” prefix; Apple trademarks in function names |

Document a single matrix in README (function → element/file → grain → join keys).

Optional future: `PRAGMA` / `duckdb_extensions` metadata is not enough — keep README + `docs/ERD.md` as source of truth.

---

## 7. Implementation sketch (engineering)

```text
path
  ├─ zip_source: list members + open export.xml / sidecars
  ├─ parse_health.c: new callbacks / pull APIs per element family
  ├─ parse_ecg_csv.c (new): header + sample pull
  └─ healthkit_export_tf.c: one TF registration block per function
```

**Hard rules (unchanged):**

- Parser/zip/ECG code: **no** DuckDB headers.
- Stable C API only.
- Fixture-first: extend `scripts/make_fixture.py` + golden CSV + SQLLogic + pytest.

**Dependency on streaming:**  
High-cardinality TFs (`metadata`, `heartbeat_points`, `ecg_samples`, filtered records) should not ship on the old bind-buffer architecture for production use. Either:

1. Land streaming (M1–M2 in STREAMING_PLAN) first, or  
2. Ship low-cardinality TFs only (`export_info`, `me`, `workout_events`, `workout_statistics`, `ecg_recordings` without samples) beforehand.

**Recommendation:** sequence low-cardinality workout children **in parallel** with streaming records; gate metadata/heartbeat/ECG samples on streaming.

---

## 8. Phased delivery

### Phase P0 — Spec freeze (short)

- [ ] Confirm Correlation policy (A + B recommended).
- [ ] Confirm whether `healthkit_me` ships or is documented-out.
- [ ] Confirm ECG header column redaction policy.
- [ ] Define soft vs hashed workout join keys.
- [ ] Extend fixture XML with: WorkoutStatistics, MetadataEntry, InstantaneousBeatsPerMinute, fake ECG CSV member.

### Phase P1 — Workout depth (v0.x minor)

- [ ] `healthkit_workout_events`
- [ ] `healthkit_workout_statistics`
- [ ] README + ERD join examples
- [ ] SQLLogic + pytest golden

**Why first:** high user value, moderate cardinality, reuses workout context already parsed.

### Phase P2 — Scan ergonomics

- [ ] Named params on `read_healthkit_export`: `types`, `start`, `end` (requires streaming pull loop ideally)
- [ ] `healthkit_export_info` + `healthkit_zip_inventory`
- [ ] Optional `healthkit_me`

### Phase P3 — Metadata + correlations

- [ ] `healthkit_metadata` (or parent-specific metadata TFs if generic is awkward)
- [ ] `healthkit_correlations` + `healthkit_correlation_members`
- [ ] Document double-count rules vs top-level records

### Phase P4 — Series + ECG

- [ ] `healthkit_heartbeat_points` (or series generic)
- [ ] `healthkit_ecg_recordings` + streaming `healthkit_ecg_samples`
- [ ] Privacy review of ECG headers

### Phase P5 — Clinical / CDA / audiogram / vision

- [ ] Only with fixtures and a maintainer willing to own schema drift

---

## 9. Compatibility and versioning

| Change | Version bump (0.x) |
|--------|--------------------|
| New table functions / nullable columns | **Minor** |
| New defaults that change row counts (e.g. nested records on) | **Minor** only if opt-in param; else avoid |
| Rename/remove columns | Avoid until 1.0; if required, **major** or long deprecation |
| Streaming behaviour with same rows | Minor (see STREAMING_PLAN) |

Community: bump `description.yml` `repo.ref` (+ `extension.version`) after each user-visible API release.

---

## 10. Testing strategy

1. **Synthetic fixture** — every new element family with non-ASCII source names and edge dates (`+1100`).
2. **SQLLogic** — smoke counts + a few literal rows.
3. **pytest golden** — CSV multisets (include full sort keys to avoid order flakes).
4. **Optional real export** — `HEALTHKIT_EXPORT_ZIP` smoke: counts only, never log PHI.
5. **Join tests** — events/statistics attach to exactly one workout on the fixture.
6. **ECG** — tiny CSV fixture in `test/data/`; no real ECG in git.

---

## 11. Non-goals (for this plan)

- Live HealthKit / Health app sync on device  
- Deduping Watch vs iPhone samples as a default semantic  
- Full FHIR/CDA clinical warehouse  
- Embedding ECG waveform UI  
- Python maps/Photos inside the extension binary  
- Wasm until streaming + size policy exist  

---

## 12. Suggested priority order (product)

1. **Workout events + statistics** — completes the workout story users already query.  
2. **Record filter parameters** — makes multi-million-row exports usable without always `COPY`ing everything.  
3. **Zip inventory + export_info** — cheap discoverability.  
4. **Correlations** — correct BP-style models without breaking top-level defaults.  
5. **Metadata** — powerful but high volume; pair with streaming + filters.  
6. **Heartbeat points + ECG** — valuable, specialised, privacy-sensitive.  
7. **Clinical / CDA / audiogram / vision** — when demand and fixtures exist.

---

## 13. Open questions

1. Should workout children use **soft keys only**, or invest early in **`workout_uid`**?  
2. Is a single `healthkit_metadata` TF acceptable, or per-parent metadata TFs?  
3. ECG: omit person-identifying header columns entirely?  
4. Do we ever emit nested Correlation records from `read_healthkit_export`, or only via correlation TFs?  
5. Parameter syntax under stable C API: named params vs trailing optional VARCHAR/BOOLEAN args — confirm against current `duckdb_extension.h` capabilities before locking README examples.  
6. Should `healthkit_store` (Python) gain matching tables in the same release train, or lag one version?

---

## 14. Success criteria (when P1 ships)

- Fixture + SQLLogic green; no change to existing TF row counts/columns.  
- Documented join from workouts → events/statistics on fixture.  
- Community README matrix lists new functions.  
- No new bind-buffer blow-ups on a large private export for P1 TFs (events/stats only).  

---

## 15. References

- Apple Health export layout (user zip): `export.xml`, `workout-routes/`, `electrocardiograms/`, optional `export_cda.xml`  
- In-repo baseline: `src/parse_health.c`, `src/healthkit_export_tf.c`, `test/data/export.xml`  
- Cousin semantics: [healthkit-to-sqlite](https://github.com/dogsheep/healthkit-to-sqlite) (nested record counting differs on purpose)  
