# /// script
# requires-python = ">=3.12"
# dependencies = [
#     "marimo",
#     "duckdb",
#     "pandas",
#     "altair",
# ]
# ///
"""Explore **new** healthkit_export APIs (P1–P4) on fixture or a real export.

**Prefer a local unsigned build** (`just debug`) so you get workout events/stats,
correlations, metadata, heartbeat points, and privacy-safe ECG helpers. Community
CDN binaries may lag until the next ``repo.ref`` publish.

Default path is the synthetic fixture. For personal data, point at a zip under
e.g. ``~/icloud/Data/apple_health_export/`` (outside git). Do not commit PHI.
"""

from __future__ import annotations

import marimo

__generated_with = "0.24.0"
app = marimo.App(width="medium")


@app.cell
def _():
    import time
    from pathlib import Path

    import altair as alt
    import duckdb
    import marimo as mo
    import pandas as pd

    return Path, alt, duckdb, mo, pd, time


@app.cell
def _(Path, mo):
    import os
    import sys

    repo_root = Path(__file__).resolve().parents[1]
    _python = str(repo_root / "python")
    if _python not in sys.path:
        sys.path.insert(0, _python)

    from healthkit_store import connect_with_healthkit_export

    fixture = repo_root / "test" / "data" / "export.zip"
    # Personal exports stay outside the repo (examples only — not committed).
    candidates = [
        os.environ.get("HEALTHKIT_EXPORT_ZIP", "").strip(),
        str(Path.home() / "icloud/Data/apple_health_export/export_2026_08_29.zip"),
        str(Path.home() / "icloud/Data/apple_health_export/export23June2025.zip"),
        str(fixture),
    ]
    default_export = next(
        (c for c in candidates if c and Path(c).expanduser().is_file()),
        str(fixture),
    )

    export_path = mo.ui.text(
        value=default_export,
        label="Health export path (.zip, .xml, or directory)",
        full_width=True,
    )
    # New APIs need local build until community republishes.
    prefer_local = mo.ui.checkbox(
        value=True,
        label="Prefer local unsigned extension (required for P1–P4 until CDN catches up)",
    )
    run_heavy = mo.ui.checkbox(
        value=False,
        label="Run heavier scans (full workout stats/events counts on large zips)",
    )
    sample_limit = mo.ui.slider(5, 50, value=15, label="Sample row limit for tables")

    mo.vstack(
        [
            mo.md(
                """
# New APIs explorer (`healthkit_export`)

Interactive smoke of **P1–P4** table functions against a fixture or **your** export.

| Area | Functions |
|------|-----------|
| Workout children | `healthkit_workout_events`, `healthkit_workout_statistics` |
| Catalogue | `healthkit_export_info`, `healthkit_zip_inventory`, `healthkit_me` |
| Correlations | `healthkit_correlations`, `healthkit_correlation_members` |
| Metadata | `healthkit_metadata` |
| HRV BPM | `healthkit_heartbeat_points` |
| ECG | `healthkit_ecg_recordings`, `healthkit_ecg_samples` |

**Privacy:** real exports stay local. ECG helpers **omit Name / Date of Birth**.
`healthkit_me` is sensitive. Prefer aggregates over dumping full tables.

**Performance:** each TF re-parses the export (bind buffers). Start with inventory
and limited samples; enable heavier counts only when needed.
"""
            ),
            export_path,
            prefer_local,
            run_heavy,
            sample_limit,
        ]
    )
    return (
        connect_with_healthkit_export,
        export_path,
        prefer_local,
        repo_root,
        run_heavy,
        sample_limit,
    )


@app.cell
def _(
    Path,
    connect_with_healthkit_export,
    export_path,
    mo,
    prefer_local,
    repo_root,
):
    zip_path = Path(export_path.value).expanduser()
    if not zip_path.exists():
        raise FileNotFoundError(f"Export not found: {zip_path}")

    # Prefer local first for new symbols; fall back to community if unchecked.
    if prefer_local.value:
        con, ext_info = connect_with_healthkit_export(
            prefer_community=False, allow_local=True
        )
    else:
        con, ext_info = connect_with_healthkit_export(
            prefer_community=True, allow_local=True
        )

    if ext_info.source == "community":
        ext_display = "community CDN"
    elif ext_info.path is not None:
        try:
            ext_display = str(ext_info.path.relative_to(repo_root))
        except ValueError:
            ext_display = str(ext_info.path)
    else:
        ext_display = ext_info.detail or ext_info.source

    # Probe whether new APIs exist on this binary.
    probe = con.execute(
        """
        SELECT function_name
        FROM duckdb_functions()
        WHERE function_name IN (
          'healthkit_workout_events',
          'healthkit_correlations',
          'healthkit_ecg_recordings',
          'healthkit_heartbeat_points'
        )
        ORDER BY 1
        """
    ).fetchdf()
    has_new = len(probe) >= 3

    mo.md(
        f"""
### Connection

| | |
|---|---|
| Extension | `{ext_display}` |
| Source | **{ext_info.source}** |
| DuckDB | `{ext_info.duckdb_version}` |
| Export | `{zip_path}` |
| Size | **{zip_path.stat().st_size / (1024**2):.1f} MiB** |
| New APIs detected | **{"yes" if has_new else "no — use local `just debug` build"}** |
"""
    )
    if not has_new:
        mo.md(
            "> **This binary lacks P1–P4 symbols.** Tick *Prefer local unsigned extension* "
            "and run `just debug` in the repo."
        )
    return con, has_new, zip_path


@app.cell
def _(con, time):
    def timed(sql: str):
        t0 = time.perf_counter()
        df = con.sql(sql).df()
        return df, time.perf_counter() - t0

    def path_lit(p) -> str:
        return p.as_posix().replace("'", "''")

    return path_lit, timed


@app.cell
def _(has_new, mo, path_lit, timed, zip_path):
    if not has_new:
        mo.md("_Skipping inventory — extension missing new APIs._")
        return

    p = path_lit(zip_path)
    inv_sql = f"""
    SELECT 'zip_members' AS kind, count(*)::BIGINT AS n
    FROM healthkit_zip_inventory('{p}')
    UNION ALL
    SELECT 'export_info_rows', count(*)::BIGINT FROM healthkit_export_info('{p}')
    UNION ALL
    SELECT 'me_rows', count(*)::BIGINT FROM healthkit_me('{p}')
    UNION ALL
    SELECT 'correlations', count(*)::BIGINT FROM healthkit_correlations('{p}')
    UNION ALL
    SELECT 'correlation_members', count(*)::BIGINT FROM healthkit_correlation_members('{p}')
    UNION ALL
    SELECT 'ecg_recordings', count(*)::BIGINT FROM healthkit_ecg_recordings('{p}')
    """
    inv_df, inv_s = timed(inv_sql)
    kinds_df, kinds_s = timed(
        f"""
        SELECT kind, count(*)::BIGINT AS n
        FROM healthkit_zip_inventory('{p}')
        GROUP BY 1 ORDER BY n DESC
        """
    )
    info_df, _ = timed(f"SELECT * FROM healthkit_export_info('{p}')")
    # Avoid dumping DOB in UI title — show columns but warn.
    me_df, _ = timed(f"SELECT * FROM healthkit_me('{p}')")

    mo.vstack(
        [
            mo.md(f"### Light inventory — **{inv_s:.2f}s** (+ kinds **{kinds_s:.2f}s**)"),
            mo.ui.table(inv_df, selection=None),
            mo.ui.table(kinds_df, selection=None),
            mo.md("#### `healthkit_export_info`"),
            mo.ui.table(info_df, selection=None),
            mo.md(
                "#### `healthkit_me` — **sensitive** (date of birth / sex / blood type)"
            ),
            mo.ui.table(me_df, selection=None),
        ]
    )
    return


@app.cell
def _(has_new, mo, path_lit, run_heavy, sample_limit, timed, zip_path):
    if not has_new:
        return

    p = path_lit(zip_path)
    lim = int(sample_limit.value)

    blocks = []

    # Correlations sample (usually small)
    corr_df, corr_s = timed(
        f"""
        SELECT type_short, start_date, end_date, source_name
        FROM healthkit_correlations('{p}')
        ORDER BY start_date DESC NULLS LAST
        LIMIT {lim}
        """
    )
    cmem_df, cmem_s = timed(
        f"""
        SELECT correlation_type_short, type_short, value, unit, start_date
        FROM healthkit_correlation_members('{p}')
        ORDER BY start_date DESC NULLS LAST
        LIMIT {lim}
        """
    )
    blocks += [
        mo.md(f"### Correlations — sample **{corr_s:.2f}s** / members **{cmem_s:.2f}s**"),
        mo.ui.table(corr_df, selection=None),
        mo.ui.table(cmem_df, selection=None),
    ]

    # Metadata sample
    meta_df, meta_s = timed(
        f"""
        SELECT parent_kind, parent_type_short, key, value, parent_start_date
        FROM healthkit_metadata('{p}')
        ORDER BY parent_start_date DESC NULLS LAST
        LIMIT {lim}
        """
    )
    meta_keys_df, meta_keys_s = timed(
        f"""
        SELECT key, count(*)::BIGINT AS n
        FROM healthkit_metadata('{p}')
        GROUP BY 1 ORDER BY n DESC
        LIMIT 25
        """
    )
    blocks += [
        mo.md(f"### Metadata — sample **{meta_s:.2f}s** / keys **{meta_keys_s:.2f}s**"),
        mo.ui.table(meta_df, selection=None),
        mo.ui.table(meta_keys_df, selection=None),
    ]

    # Workout children
    if run_heavy.value:
        we_sql = f"SELECT count(*)::BIGINT AS n FROM healthkit_workout_events('{p}')"
        ws_sql = f"SELECT count(*)::BIGINT AS n FROM healthkit_workout_statistics('{p}')"
        we_n, we_s = timed(we_sql)
        ws_n, ws_s = timed(ws_sql)
        blocks.append(
            mo.md(
                f"### Workout children counts — events **{we_n.iloc[0,0]:,}** "
                f"({we_s:.1f}s), stats **{ws_n.iloc[0,0]:,}** ({ws_s:.1f}s)"
            )
        )

    ev_df, ev_s = timed(
        f"""
        SELECT event_type_short, event_date, workout_activity_type_short, workout_start_date
        FROM healthkit_workout_events('{p}')
        ORDER BY event_date DESC NULLS LAST
        LIMIT {lim}
        """
    )
    st_df, st_s = timed(
        f"""
        SELECT stat_type_short, sum, average, unit, workout_activity_type_short, workout_start_date
        FROM healthkit_workout_statistics('{p}')
        ORDER BY workout_start_date DESC NULLS LAST
        LIMIT {lim}
        """
    )
    blocks += [
        mo.md(f"### Workout events / stats samples — **{ev_s:.2f}s** / **{st_s:.2f}s**"),
        mo.ui.table(ev_df, selection=None),
        mo.ui.table(st_df, selection=None),
    ]

    mo.vstack(blocks)
    return


@app.cell
def _(alt, has_new, mo, path_lit, sample_limit, timed, zip_path):
    if not has_new:
        return

    p = path_lit(zip_path)
    lim = int(sample_limit.value)

    hb_df, hb_s = timed(
        f"""
        SELECT bpm, time_text, parent_type_short, parent_start_date, parent_value, parent_unit
        FROM healthkit_heartbeat_points('{p}')
        ORDER BY parent_start_date DESC NULLS LAST
        LIMIT {lim}
        """
    )
    hb_count_df, hb_cs = timed(
        f"SELECT count(*)::BIGINT AS n FROM healthkit_heartbeat_points('{p}')"
    )

    ecgr_df, ecgr_s = timed(
        f"""
        SELECT ecg_path, recorded_at, classification, device, sample_rate_hz, lead, unit, n_samples
        FROM healthkit_ecg_recordings('{p}')
        ORDER BY recorded_at DESC NULLS LAST
        LIMIT {lim}
        """
    )
    # Only pull samples for the first recording path if any — avoids multi-ECG blow-up.
    sample_blocks = [
        mo.md(
            f"### Heartbeat points — count **{hb_count_df.iloc[0,0]:,}** "
            f"({hb_cs:.2f}s), sample **{hb_s:.2f}s**"
        ),
        mo.ui.table(hb_df, selection=None),
        mo.md(
            f"### ECG recordings — **{ecgr_s:.2f}s**  \n"
            "_No Name / Date of Birth columns by design._"
        ),
        mo.ui.table(ecgr_df, selection=None),
    ]

    if len(ecgr_df) and "ecg_path" in ecgr_df.columns:
        first = str(ecgr_df.iloc[0]["ecg_path"]).replace("'", "''")
        samp_df, samp_s = timed(
            f"""
            SELECT sample_index, value_uv
            FROM healthkit_ecg_samples('{p}', ecg_path := '{first}')
            ORDER BY sample_index
            LIMIT 500
            """
        )
        sample_blocks.append(mo.md(f"#### ECG samples (first recording, ≤500) — **{samp_s:.2f}s**"))
        sample_blocks.append(mo.ui.table(samp_df.head(30), selection=None))
        if len(samp_df):
            chart = (
                alt.Chart(samp_df)
                .mark_line(opacity=0.85)
                .encode(
                    x=alt.X("sample_index:Q", title="sample_index"),
                    y=alt.Y("value_uv:Q", title="µV"),
                    tooltip=["sample_index", "value_uv"],
                )
                .properties(height=220, title="ECG waveform preview (filtered path)")
            )
            sample_blocks.append(chart)

    if len(hb_df) and "bpm" in hb_df.columns:
        sample_blocks.append(
            alt.Chart(hb_df)
            .mark_circle(size=60)
            .encode(
                x=alt.X("bpm:Q", title="bpm"),
                y=alt.Y("parent_value:Q", title="parent HRV value"),
                tooltip=["bpm", "time_text", "parent_start_date", "parent_value"],
            )
            .properties(height=220, title="Sample IBPM vs parent HRV")
        )

    mo.vstack(sample_blocks)
    return


@app.cell
def _(has_new, mo, path_lit, timed, zip_path):
    if not has_new:
        return

    p = path_lit(zip_path)
    # Cheap filtered record demo (not full multi-million scan unless user path is tiny)
    hr_df, hr_s = timed(
        f"""
        SELECT type_short, start_date, value, unit, source_name
        FROM read_healthkit_export('{p}', types := 'HeartRate')
        ORDER BY start_date DESC NULLS LAST
        LIMIT 20
        """
    )
    mo.vstack(
        [
            mo.md(
                f"### Filtered records (`types := 'HeartRate'`) — **{hr_s:.2f}s**  \n"
                "_Still a full XML walk today; filter only reduces rows kept after parse._"
            ),
            mo.ui.table(hr_df, selection=None),
            mo.md(
                """
### Next steps

- Materialise slices: `COPY (SELECT … FROM read_healthkit_export(..., types := '…')) TO '….parquet'`
- Local walk DB: `just build-db export_zip=/path/to/export.zip`
- Docs: `docs/QUICKSTART.md`, `docs/ERD.md`, `docs/ADDITIONAL_API_PLAN.md`
"""
            ),
        ]
    )
    return


if __name__ == "__main__":
    app.run()
