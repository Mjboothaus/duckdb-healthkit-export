# Streaming plan — `healthkit_export`

Last updated: 2026-09-19.  
Status: **design / not started**. Target: **v0.4.0** (API-compatible; behaviour and memory profile change).

Related: [DESIGN.md](DESIGN.md) · [ROADMAP.md](ROADMAP.md) · [RELEASE_NOTES.md](RELEASE_NOTES.md) · [BRANDING.md](BRANDING.md)

---

## 1. Problem

Today each table function (**bind**) opens the export, runs the full XML/GPX parse, and **stores every row in heap** (`bind_data->rows`). **Execute** only walks that array into DuckDB chunks.

Consequences:

| Issue | Effect |
|-------|--------|
| Peak RAM ≈ full result set | Multi‑GB `export.xml` / large route GPX → OOM or multi‑GB RSS |
| Bind latency | Long pause before first row; hard to cancel mid-scan |
| Re-scan cost | Every TF invocation re-parses the whole export |
| Wasm / browser | Effectively blocked for real exports |
| Filters | `WHERE` runs *after* materialisation (no pushdown yet) |

The **parser** is already callback-oriented (`ah_parse_xml_filep`, `on_record`, …). The **table-function layer** is not streaming.

---

## 2. Goals

1. **Bounded peak memory** per scan (order: a few DuckDB vectors + a small parse window, not the full table).
2. **First rows available** without finishing the whole file (progressive execute).
3. **Same SQL shapes** and column schemas (no rename; optional new parameters later).
4. **Correctness** vs current fixture / SQLLogic / healthkit-to-sqlite compare (top-level records).
5. **Cancellable** scans where the C API allows (cooperative check between chunks).
6. Lay groundwork for **named filters** (`types`, `start`, `end`) and eventual **Wasm**.

### Non-goals (this plan)

- Changing community extension id or branding.
- Full XQuery/XPath over the export.
- Exact-once incremental DB append (that stays in `healthkit-store` / `build-db`).
- DuckDB 2.0-only APIs (prefer stable C API; note 2.0 only if required).

---

## 3. Current architecture (as-is)

```text
  path (.zip | dir | .xml)
           │
           ▼
  ah_xml_source_open ──► FILE* (maybe temp inflate)
           │
           ▼
  ah_parse_xml_filep + callbacks
           │
           ▼
  Bind: collect ALL rows → bind_data.rows[]
           │
           ▼
  Init: offset = 0
           │
           ▼
  Function: fill data_chunk from rows[offset …]
```

Applies to:

- `read_healthkit_export`
- `healthkit_workouts`
- `healthkit_activity_summaries`
- `healthkit_workout_routes`
- `healthkit_workout_route_points` (plus per-route GPX open)

Parser already streams **bytes → callbacks**. Bind **aggregates** callbacks into a giant array.

---

## 4. Target architecture (to-be)

```text
  path
    │
    ▼
  Bind: validate path, open source (or defer open), set schema + approx cardinality unknown
    │
    ▼
  Init (per parallel thread / single scan): open FILE*, create parse state machine
    │
    ▼
  Function (each call):
       resume parser until chunk full OR EOF OR error
       emit vectors
       keep parse cursor in init_data
    │
    ▼
  Destroy: close source, free window buffers only
```

### Cardinality

- Prefer **unknown** cardinality in bind (`duckdb_bind_set_cardinality` with `false` exact flag), or a cheap upper bound only if free.
- Avoid a full pre-count pass (defeats streaming).

### Parallelism

- **Phase 1:** single-threaded scan (`duckdb_bind_set_max_threads` / init = 1) — correct and simpler.
- **Phase 2 (optional):** partition by file regions only if we add an index or multi-member zip strategy; XML is inherently sequential → default stay single-threaded.

---

## 5. Design options

### Option A — Pull parser in execute (recommended)

Turn the XML scan into a **resumable state machine**:

- `init_data` holds: `ah_xml_source*`, parser cursor (buffer offset, element stack, current tag state), small ring/queue of pending rows (≤ chunk size).
- Each `Function` call: `parse_more()` until `queue.size >= STANDARD_VECTOR_SIZE` or EOF.
- Emit chunk from queue; leave remainder for next call.

**Pros:** True streaming; natural fit for DuckDB execute loop.  
**Cons:** Largest refactor of `parse_health.c` (today likely one-shot to EOF).

### Option B — Spill bind rows to temp storage

Keep collect-all semantics but write rows to **temp file / DuckDB buffer files** instead of RAM.

**Pros:** Smaller code change.  
**Cons:** Still full scan before first row; disk thrash; weak for Wasm; not “streaming UX”.

### Option C — Producer thread + bounded queue

Background thread parses; execute thread pulls chunks.

**Pros:** Can overlap parse and sink.  
**Cons:** Threading + C API lifetime complexity; harder cancellation; overkill for Phase 1.

**Decision:** **Option A** for Phase 1–2. Option B only as emergency mitigation. Option C deferred.

---

## 6. Phased delivery

### Phase 0 — Instrumentation (short)

- Log/measure on a real export (local only): bind RSS, bind wall time, row counts per TF.
- Add a **debug** counter in bind (optional compile flag) for peak `rows` capacity.
- Document baseline in this file or a private note (no PHI in git).

**Exit:** numbers for “before” on at least one multi‑hundred‑MB export.

### Phase 1 — Stream `read_healthkit_export` only

**Scope:** top-level `<Record>` path only (highest row count).

1. Introduce `ah_xml_parser` (or equivalent) with:
   - `create(fp)`
   - `pull_records(out_buf, max_n) → n | EOF | error`
   - `destroy`
2. Refactor existing `ah_parse_xml_filep` to share the same core (callbacks become a thin loop over `pull_*` **or** reverse: streaming TF drives pull API).
3. Change `ReadHealthkitExportBind` to:
   - resolve path, open `ah_xml_source` **or** store path string only
   - set columns; cardinality unknown
   - **do not** allocate full `rows[]`
4. `ReadHealthkitExportInit`: create parser state on the source.
5. `ReadHealthkitExportFunction`: pull ≤ vector size; convert `ah_record` → vectors (reuse today’s assign helpers).
6. Tests:
   - existing SQLLogic + fixture pytest must pass
   - new test: scan fixture twice in one connection (no leak)
   - optional: artificial low memory / chunk-count assert in unit test with tiny fixture

**Exit:** fixture parity; bind RSS on large export drops dramatically for records TF.

### Phase 2 — Stream remaining TFs

Same pattern for:

| Function | Notes |
|----------|--------|
| `healthkit_workouts` | Lower volume; still full-file XML walk |
| `healthkit_activity_summaries` | Same |
| `healthkit_workout_routes` | Same XML; lighter rows |
| `healthkit_workout_route_points` | **Nested:** for each route, stream GPX points without loading entire GPX into RAM |

GPX: extend `parse_gpx` with pull/resume API (or chunked file read with pending `trkpt` queue).

**Exit:** all five TFs stream; no full-table `rows[]` in bind for default paths.

### Phase 3 — Filter pushdown (builds on streaming)

Named parameters (stable C API permitting):

- `types` — list/VARCHAR filter on `type_short` / prefix
- `start` / `end` — on `start_date` (parsed once per row; skip emit if out of range)

Pushdown happens **in the pull loop** (do not enqueue filtered-out rows).

Document interaction with SQL `WHERE` (redundant but correct).

### Phase 4 — Zip inflate streaming (optional stretch)

Today DEFLATE members may inflate to a **temp file** then parse. Improve to:

- zlib inflate window + XML pull on the same pipe, **or**
- memory-capped temp spill with explicit limit

Reduces disk and improves first-byte latency on huge zips.

### Phase 5 — Wasm re-evaluation

Only after Phase 1–2:

- fixture + small exports in DuckDB-Wasm
- document hard limits (e.g. recommend &lt; N MB uncompressed in browser)
- community `excluded_platforms` Wasm still optional

---

## 7. API and compatibility

| Surface | Policy |
|---------|--------|
| Function names / columns | **Unchanged** |
| Row order | Best-effort document-order (same as today); do not promise global sort |
| Errors | Prefer fail the scan with clear message; partial chunks already returned stay (DuckDB semantics) |
| Extension version | Bump **minor** (e.g. 0.4.0) when default path is streaming |
| C API target | Keep **stable** extension API v1.2.0 unless a hard blocker appears |

---

## 8. Implementation sketch (Phase 1)

### 8.1 Parser pull API (illustrative)

```c
typedef struct ah_record_parser ah_record_parser;

ah_record_parser *ah_record_parser_create(FILE *fp, char *err, size_t err_len);
/* Returns 1 if a record was written, 0 on EOF, -1 on error. */
int ah_record_parser_next(ah_record_parser *p, ah_record *out);
void ah_record_parser_destroy(ah_record_parser *p);
```

Internally: keep a read buffer, tag stack depth, “inside Correlation?” flag (preserve top-level-only semantics).

### 8.2 Bind / init / function responsibilities

| Stage | Owns |
|-------|------|
| Bind | path string, optional opened source, schema |
| Init | parser instance, source lifetime, error flag |
| Function | convert next N records → chunk; set size; propagate errors via `duckdb_function_set_error` |

### 8.3 Memory budget (targets)

| Resource | Target |
|----------|--------|
| Pending row queue | ≤ 1–2 × `STANDARD_VECTOR_SIZE` |
| XML read buffer | fixed (e.g. 64–256 KiB), grow only if a single tag exceeds |
| Peak RSS vs v0.3 | ≥ 10× reduction on multi‑million-row exports (measure in Phase 0) |

---

## 9. Testing strategy

1. **SQLLogic** — existing `test/sql/*.test` unchanged expectations.
2. **pytest-ext** — golden + healthkit-to-sqlite top-level compare.
3. **Chunk stress** — force small output chunk sizes if C API allows; assert many execute calls.
4. **Resource** — optional macOS `sample` / `/usr/bin/time -l` script (gitignored paths only).
5. **Regression** — nested Correlation still skipped; dates/timezones unchanged.
6. **Multi-TF** — opening workouts + records on same zip in one query plan (two scans).

---

## 10. Risks and mitigations

| Risk | Mitigation |
|------|------------|
| Resumable XML is subtle (split tags across reads) | Keep a carry buffer; fuzz with random chunk sizes on fixture |
| Temp zip inflate still huge | Phase 4; document “prefer unzipped export.xml on constrained hosts” interim |
| Cardinality unknown hurts some optimizers | Acceptable; filters later reduce work |
| Parallel TF assumes independent inits | Force max 1 thread until proven |
| Behaviour change timing (errors later in scan) | Document; keep fail-loud |

---

## 11. Work breakdown (engineering checklist)

After approval, track in issues/PRs (not necessarily all at once):

1. [ ] Phase 0 baselines on one large private export  
2. [ ] `ah_record_parser_*` pull API + unit tests on fixture  
3. [ ] Wire `read_healthkit_export` to pull API  
4. [ ] Memory/time compare write-up  
5. [ ] Stream workouts + activity summaries  
6. [ ] Stream routes + route_points (GPX pull)  
7. [ ] Optional named filter parameters  
8. [ ] Zip inflate streaming spike  
9. [ ] Docs: README performance section rewrite  
10. [ ] Release **v0.4.0** + community ref bump  
11. [ ] Revisit Wasm exclusion  

---

## 12. Suggested milestones

| Milestone | Deliverable | Version bump |
|-----------|-------------|--------------|
| M0 | Baselines + design sign-off | — |
| M1 | Streaming records TF | 0.4.0-alpha / main |
| M2 | All TFs streaming | **0.4.0** community |
| M3 | Filter pushdown | 0.4.x or 0.5.0 |
| M4 | Inflate streaming + Wasm spike | roadmap |

---

## 13. Success criteria (M2)

- Fixture and SQLLogic: **100% parity** with v0.3.x results.  
- Large export: peak RSS for `read_healthkit_export` **≪** result size (order-of-magnitude win).  
- First chunk latency **≪** full-scan time.  
- No public API break.  
- CI green on native platforms (macOS / Linux / Windows); Wasm still optional.

---

## 14. Open questions

1. Should bind open the zip once and **reject** parallel inits, or allow N independent full scans (simpler, more IO)?  
2. Do we expose a setting `healthkit_export_scan_buffer_kb`?  
3. Is temp-file inflate acceptable through M2 if pull-parse is streaming? (**Yes** default.)  
4. Community: ship streaming as default only, or gate with a temp parameter `streaming=true` for one release? (**Recommend default on**, no gate, if tests are solid.)

---

## 15. References in-tree

- Bind collect pattern: `src/healthkit_export_tf.c` (`ReadHealthkitExportBind`, `*BindData.rows`)  
- Parser callbacks: `src/parse_health.h` (`ah_parse_callbacks`, `ah_parse_xml_filep`)  
- Zip/temp: `src/zip_source.c` (`inflate_member_to_temp`, Windows temp paths)  
- Performance honesty: README “Performance (real exports)” · ROADMAP performance list  
