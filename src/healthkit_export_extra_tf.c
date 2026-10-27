#include "duckdb_extension.h"

#include "healthkit_export_tf.h"
#include "parse_health.h"
#include "zip_source.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

DUCKDB_EXTENSION_EXTERN

/* Shared helpers (duplicated lightly to keep this file self-contained). */

static void DestroyScanInit(void *ptr) {
	duckdb_free(ptr);
}

typedef struct {
	size_t offset;
} scan_init_data;

static void ScanInit(duckdb_init_info info) {
	scan_init_data *init = (scan_init_data *)duckdb_malloc(sizeof(scan_init_data));
	if (!init) {
		duckdb_init_set_error(info, "out of memory");
		return;
	}
	init->offset = 0;
	duckdb_init_set_init_data(info, init, DestroyScanInit);
}

static void AssignVarchar(duckdb_vector vec, idx_t row, const char *s) {
	if (!s) {
		s = "";
	}
	duckdb_vector_assign_string_element(vec, row, s);
}

static void AssignTimestamp(duckdb_vector vec, uint64_t *validity, idx_t row, const char *apple_date) {
	duckdb_timestamp *data = (duckdb_timestamp *)duckdb_vector_get_data(vec);
	int64_t micros = 0;
	if (apple_date && apple_date[0] && ah_parse_apple_date(apple_date, NULL, 0, &micros)) {
		data[row].micros = micros;
	} else {
		duckdb_validity_set_row_invalid(validity, row);
	}
}

static void AssignOptionalDouble(duckdb_vector vec, uint64_t *validity, idx_t row, bool has, double value) {
	double *data = (double *)duckdb_vector_get_data(vec);
	if (has) {
		data[row] = value;
	} else {
		duckdb_validity_set_row_invalid(validity, row);
	}
}

static char *BindPathOrError(duckdb_bind_info info, const char *fname) {
	if (duckdb_bind_get_parameter_count(info) < 1) {
		char msg[128];
		snprintf(msg, sizeof(msg), "%s requires a path argument", fname);
		duckdb_bind_set_error(info, msg);
		return NULL;
	}
	duckdb_value path_val = duckdb_bind_get_parameter(info, 0);
	if (!path_val) {
		duckdb_bind_set_error(info, "missing path");
		return NULL;
	}
	char *path_cstr = duckdb_get_varchar(path_val);
	duckdb_destroy_value(&path_val);
	if (!path_cstr || !path_cstr[0]) {
		if (path_cstr) {
			duckdb_free(path_cstr);
		}
		duckdb_bind_set_error(info, "empty path");
		return NULL;
	}
	char *out = strdup(path_cstr);
	duckdb_free(path_cstr);
	if (!out) {
		duckdb_bind_set_error(info, "out of memory");
		return NULL;
	}
	return out;
}

static void OnRecordIgnore(const ah_record *row, void *userdata) {
	(void)row;
	(void)userdata;
}
static void OnWorkoutIgnore(const ah_workout *row, void *userdata) {
	(void)row;
	(void)userdata;
}
static void OnSummaryIgnore(const ah_activity_summary *row, void *userdata) {
	(void)row;
	(void)userdata;
}
static void OnRouteIgnore(const ah_workout_route *row, void *userdata) {
	(void)row;
	(void)userdata;
}

/* -------------------------------------------------------------------------- */
/* healthkit_workout_events                                                   */
/* -------------------------------------------------------------------------- */

typedef struct {
	ah_workout_event *rows;
	size_t count;
	size_t capacity;
	char *path;
	char *filename;
} events_bind_data;

static void DestroyEventsBind(void *ptr) {
	events_bind_data *b = (events_bind_data *)ptr;
	if (!b) {
		return;
	}
	free(b->rows);
	free(b->path);
	free(b->filename);
	duckdb_free(b);
}

static void OnWorkoutEventCollect(const ah_workout_event *row, void *userdata) {
	events_bind_data *b = (events_bind_data *)userdata;
	if (b->count >= b->capacity) {
		size_t ncap = b->capacity ? b->capacity * 2 : 16;
		ah_workout_event *nr = (ah_workout_event *)realloc(b->rows, ncap * sizeof(ah_workout_event));
		if (!nr) {
			return;
		}
		b->rows = nr;
		b->capacity = ncap;
	}
	b->rows[b->count++] = *row;
}

static void EventsBind(duckdb_bind_info info) {
	duckdb_logical_type varchar_type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	duckdb_logical_type double_type = duckdb_create_logical_type(DUCKDB_TYPE_DOUBLE);
	duckdb_logical_type ts_type = duckdb_create_logical_type(DUCKDB_TYPE_TIMESTAMP_TZ);

	duckdb_bind_add_result_column(info, "event_type", varchar_type);
	duckdb_bind_add_result_column(info, "event_type_short", varchar_type);
	duckdb_bind_add_result_column(info, "event_date", ts_type);
	duckdb_bind_add_result_column(info, "duration", double_type);
	duckdb_bind_add_result_column(info, "duration_unit", varchar_type);
	duckdb_bind_add_result_column(info, "workout_activity_type", varchar_type);
	duckdb_bind_add_result_column(info, "workout_activity_type_short", varchar_type);
	duckdb_bind_add_result_column(info, "workout_start_date", ts_type);
	duckdb_bind_add_result_column(info, "workout_end_date", ts_type);
	duckdb_bind_add_result_column(info, "workout_source_name", varchar_type);
	duckdb_bind_add_result_column(info, "filename", varchar_type);

	duckdb_destroy_logical_type(&varchar_type);
	duckdb_destroy_logical_type(&double_type);
	duckdb_destroy_logical_type(&ts_type);

	char *path = BindPathOrError(info, "healthkit_workout_events");
	if (!path) {
		return;
	}
	events_bind_data *bind = (events_bind_data *)duckdb_malloc(sizeof(events_bind_data));
	if (!bind) {
		free(path);
		duckdb_bind_set_error(info, "out of memory");
		return;
	}
	memset(bind, 0, sizeof(*bind));
	bind->path = path;

	char err[256];
	err[0] = '\0';
	ah_xml_source *src = ah_xml_source_open(bind->path, err, sizeof(err));
	if (!src) {
		char msg[320];
		snprintf(msg, sizeof(msg), "healthkit_workout_events: cannot open '%s': %s", bind->path,
		         err[0] ? err : "error");
		DestroyEventsBind(bind);
		duckdb_bind_set_error(info, msg);
		return;
	}
	bind->filename = strdup(ah_xml_source_filename(src));

	ah_parse_callbacks cb = {
	    .on_record = OnRecordIgnore,
	    .on_workout = OnWorkoutIgnore,
	    .on_activity_summary = OnSummaryIgnore,
	    .on_workout_route = OnRouteIgnore,
	    .on_workout_event = OnWorkoutEventCollect,
	    .userdata = bind,
	};
	ah_parse_stats stats;
	memset(&stats, 0, sizeof(stats));
	int rc = ah_parse_xml_filep(ah_xml_source_file(src), &cb, &stats);
	ah_xml_source_close(src);
	if (rc != 0) {
		DestroyEventsBind(bind);
		duckdb_bind_set_error(info, "healthkit_workout_events: parse failed");
		return;
	}
	duckdb_bind_set_bind_data(info, bind, DestroyEventsBind);
	duckdb_bind_set_cardinality(info, (idx_t)bind->count, true);
}

static void EventsFunction(duckdb_function_info info, duckdb_data_chunk output) {
	events_bind_data *bind = (events_bind_data *)duckdb_function_get_bind_data(info);
	scan_init_data *init = (scan_init_data *)duckdb_function_get_init_data(info);
	if (!bind || !init || init->offset >= bind->count) {
		duckdb_data_chunk_set_size(output, 0);
		return;
	}
	const idx_t vector_size = duckdb_vector_size();
	size_t remaining = bind->count - init->offset;
	idx_t n = remaining > (size_t)vector_size ? vector_size : (idx_t)remaining;

	duckdb_vector v_etype = duckdb_data_chunk_get_vector(output, 0);
	duckdb_vector v_eshort = duckdb_data_chunk_get_vector(output, 1);
	duckdb_vector v_edate = duckdb_data_chunk_get_vector(output, 2);
	duckdb_vector v_dur = duckdb_data_chunk_get_vector(output, 3);
	duckdb_vector v_dur_u = duckdb_data_chunk_get_vector(output, 4);
	duckdb_vector v_wtype = duckdb_data_chunk_get_vector(output, 5);
	duckdb_vector v_wshort = duckdb_data_chunk_get_vector(output, 6);
	duckdb_vector v_wstart = duckdb_data_chunk_get_vector(output, 7);
	duckdb_vector v_wend = duckdb_data_chunk_get_vector(output, 8);
	duckdb_vector v_wsrc = duckdb_data_chunk_get_vector(output, 9);
	duckdb_vector v_file = duckdb_data_chunk_get_vector(output, 10);

	duckdb_vector_ensure_validity_writable(v_edate);
	duckdb_vector_ensure_validity_writable(v_dur);
	duckdb_vector_ensure_validity_writable(v_wstart);
	duckdb_vector_ensure_validity_writable(v_wend);
	uint64_t *edate_v = duckdb_vector_get_validity(v_edate);
	uint64_t *dur_v = duckdb_vector_get_validity(v_dur);
	uint64_t *wstart_v = duckdb_vector_get_validity(v_wstart);
	uint64_t *wend_v = duckdb_vector_get_validity(v_wend);
	const char *filename = bind->filename ? bind->filename : "";

	for (idx_t i = 0; i < n; i++) {
		const ah_workout_event *row = &bind->rows[init->offset + i];
		AssignVarchar(v_etype, i, row->event_type);
		AssignVarchar(v_eshort, i, row->event_type_short);
		AssignTimestamp(v_edate, edate_v, i, row->event_date);
		AssignOptionalDouble(v_dur, dur_v, i, row->has_duration, row->duration);
		AssignVarchar(v_dur_u, i, row->duration_unit);
		AssignVarchar(v_wtype, i, row->workout_activity_type);
		AssignVarchar(v_wshort, i, row->workout_activity_type_short);
		AssignTimestamp(v_wstart, wstart_v, i, row->workout_start_date);
		AssignTimestamp(v_wend, wend_v, i, row->workout_end_date);
		AssignVarchar(v_wsrc, i, row->workout_source_name);
		AssignVarchar(v_file, i, filename);
	}
	init->offset += n;
	duckdb_data_chunk_set_size(output, n);
}

void RegisterHealthkitWorkoutEventsFunction(duckdb_connection connection) {
	duckdb_table_function function = duckdb_create_table_function();
	duckdb_table_function_set_name(function, "healthkit_workout_events");
	duckdb_logical_type varchar_type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	duckdb_table_function_add_parameter(function, varchar_type);
	duckdb_destroy_logical_type(&varchar_type);
	duckdb_table_function_set_bind(function, EventsBind);
	duckdb_table_function_set_init(function, ScanInit);
	duckdb_table_function_set_function(function, EventsFunction);
	duckdb_register_table_function(connection, function);
	duckdb_destroy_table_function(&function);
}

/* -------------------------------------------------------------------------- */
/* healthkit_workout_statistics                                               */
/* -------------------------------------------------------------------------- */

typedef struct {
	ah_workout_statistic *rows;
	size_t count;
	size_t capacity;
	char *path;
	char *filename;
} stats_bind_data;

static void DestroyStatsBind(void *ptr) {
	stats_bind_data *b = (stats_bind_data *)ptr;
	if (!b) {
		return;
	}
	free(b->rows);
	free(b->path);
	free(b->filename);
	duckdb_free(b);
}

static void OnWorkoutStatCollect(const ah_workout_statistic *row, void *userdata) {
	stats_bind_data *b = (stats_bind_data *)userdata;
	if (b->count >= b->capacity) {
		size_t ncap = b->capacity ? b->capacity * 2 : 16;
		ah_workout_statistic *nr = (ah_workout_statistic *)realloc(b->rows, ncap * sizeof(ah_workout_statistic));
		if (!nr) {
			return;
		}
		b->rows = nr;
		b->capacity = ncap;
	}
	b->rows[b->count++] = *row;
}

static void StatsBind(duckdb_bind_info info) {
	duckdb_logical_type varchar_type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	duckdb_logical_type double_type = duckdb_create_logical_type(DUCKDB_TYPE_DOUBLE);
	duckdb_logical_type ts_type = duckdb_create_logical_type(DUCKDB_TYPE_TIMESTAMP_TZ);

	duckdb_bind_add_result_column(info, "stat_type", varchar_type);
	duckdb_bind_add_result_column(info, "stat_type_short", varchar_type);
	duckdb_bind_add_result_column(info, "start_date", ts_type);
	duckdb_bind_add_result_column(info, "end_date", ts_type);
	duckdb_bind_add_result_column(info, "unit", varchar_type);
	duckdb_bind_add_result_column(info, "sum", double_type);
	duckdb_bind_add_result_column(info, "minimum", double_type);
	duckdb_bind_add_result_column(info, "maximum", double_type);
	duckdb_bind_add_result_column(info, "average", double_type);
	duckdb_bind_add_result_column(info, "workout_activity_type", varchar_type);
	duckdb_bind_add_result_column(info, "workout_activity_type_short", varchar_type);
	duckdb_bind_add_result_column(info, "workout_start_date", ts_type);
	duckdb_bind_add_result_column(info, "workout_end_date", ts_type);
	duckdb_bind_add_result_column(info, "workout_source_name", varchar_type);
	duckdb_bind_add_result_column(info, "filename", varchar_type);

	duckdb_destroy_logical_type(&varchar_type);
	duckdb_destroy_logical_type(&double_type);
	duckdb_destroy_logical_type(&ts_type);

	char *path = BindPathOrError(info, "healthkit_workout_statistics");
	if (!path) {
		return;
	}
	stats_bind_data *bind = (stats_bind_data *)duckdb_malloc(sizeof(stats_bind_data));
	if (!bind) {
		free(path);
		duckdb_bind_set_error(info, "out of memory");
		return;
	}
	memset(bind, 0, sizeof(*bind));
	bind->path = path;

	char err[256];
	err[0] = '\0';
	ah_xml_source *src = ah_xml_source_open(bind->path, err, sizeof(err));
	if (!src) {
		char msg[320];
		snprintf(msg, sizeof(msg), "healthkit_workout_statistics: cannot open '%s': %s", bind->path,
		         err[0] ? err : "error");
		DestroyStatsBind(bind);
		duckdb_bind_set_error(info, msg);
		return;
	}
	bind->filename = strdup(ah_xml_source_filename(src));

	ah_parse_callbacks cb = {
	    .on_record = OnRecordIgnore,
	    .on_workout = OnWorkoutIgnore,
	    .on_activity_summary = OnSummaryIgnore,
	    .on_workout_route = OnRouteIgnore,
	    .on_workout_statistic = OnWorkoutStatCollect,
	    .userdata = bind,
	};
	ah_parse_stats stats;
	memset(&stats, 0, sizeof(stats));
	int rc = ah_parse_xml_filep(ah_xml_source_file(src), &cb, &stats);
	ah_xml_source_close(src);
	if (rc != 0) {
		DestroyStatsBind(bind);
		duckdb_bind_set_error(info, "healthkit_workout_statistics: parse failed");
		return;
	}
	duckdb_bind_set_bind_data(info, bind, DestroyStatsBind);
	duckdb_bind_set_cardinality(info, (idx_t)bind->count, true);
}

static void StatsFunction(duckdb_function_info info, duckdb_data_chunk output) {
	stats_bind_data *bind = (stats_bind_data *)duckdb_function_get_bind_data(info);
	scan_init_data *init = (scan_init_data *)duckdb_function_get_init_data(info);
	if (!bind || !init || init->offset >= bind->count) {
		duckdb_data_chunk_set_size(output, 0);
		return;
	}
	const idx_t vector_size = duckdb_vector_size();
	size_t remaining = bind->count - init->offset;
	idx_t n = remaining > (size_t)vector_size ? vector_size : (idx_t)remaining;

	duckdb_vector v_stype = duckdb_data_chunk_get_vector(output, 0);
	duckdb_vector v_sshort = duckdb_data_chunk_get_vector(output, 1);
	duckdb_vector v_start = duckdb_data_chunk_get_vector(output, 2);
	duckdb_vector v_end = duckdb_data_chunk_get_vector(output, 3);
	duckdb_vector v_unit = duckdb_data_chunk_get_vector(output, 4);
	duckdb_vector v_sum = duckdb_data_chunk_get_vector(output, 5);
	duckdb_vector v_min = duckdb_data_chunk_get_vector(output, 6);
	duckdb_vector v_max = duckdb_data_chunk_get_vector(output, 7);
	duckdb_vector v_avg = duckdb_data_chunk_get_vector(output, 8);
	duckdb_vector v_wtype = duckdb_data_chunk_get_vector(output, 9);
	duckdb_vector v_wshort = duckdb_data_chunk_get_vector(output, 10);
	duckdb_vector v_wstart = duckdb_data_chunk_get_vector(output, 11);
	duckdb_vector v_wend = duckdb_data_chunk_get_vector(output, 12);
	duckdb_vector v_wsrc = duckdb_data_chunk_get_vector(output, 13);
	duckdb_vector v_file = duckdb_data_chunk_get_vector(output, 14);

	duckdb_vector_ensure_validity_writable(v_start);
	duckdb_vector_ensure_validity_writable(v_end);
	duckdb_vector_ensure_validity_writable(v_sum);
	duckdb_vector_ensure_validity_writable(v_min);
	duckdb_vector_ensure_validity_writable(v_max);
	duckdb_vector_ensure_validity_writable(v_avg);
	duckdb_vector_ensure_validity_writable(v_wstart);
	duckdb_vector_ensure_validity_writable(v_wend);
	uint64_t *start_v = duckdb_vector_get_validity(v_start);
	uint64_t *end_v = duckdb_vector_get_validity(v_end);
	uint64_t *sum_v = duckdb_vector_get_validity(v_sum);
	uint64_t *min_v = duckdb_vector_get_validity(v_min);
	uint64_t *max_v = duckdb_vector_get_validity(v_max);
	uint64_t *avg_v = duckdb_vector_get_validity(v_avg);
	uint64_t *wstart_v = duckdb_vector_get_validity(v_wstart);
	uint64_t *wend_v = duckdb_vector_get_validity(v_wend);
	const char *filename = bind->filename ? bind->filename : "";

	for (idx_t i = 0; i < n; i++) {
		const ah_workout_statistic *row = &bind->rows[init->offset + i];
		AssignVarchar(v_stype, i, row->stat_type);
		AssignVarchar(v_sshort, i, row->stat_type_short);
		AssignTimestamp(v_start, start_v, i, row->start_date);
		AssignTimestamp(v_end, end_v, i, row->end_date);
		AssignVarchar(v_unit, i, row->unit);
		AssignOptionalDouble(v_sum, sum_v, i, row->has_sum, row->sum);
		AssignOptionalDouble(v_min, min_v, i, row->has_minimum, row->minimum);
		AssignOptionalDouble(v_max, max_v, i, row->has_maximum, row->maximum);
		AssignOptionalDouble(v_avg, avg_v, i, row->has_average, row->average);
		AssignVarchar(v_wtype, i, row->workout_activity_type);
		AssignVarchar(v_wshort, i, row->workout_activity_type_short);
		AssignTimestamp(v_wstart, wstart_v, i, row->workout_start_date);
		AssignTimestamp(v_wend, wend_v, i, row->workout_end_date);
		AssignVarchar(v_wsrc, i, row->workout_source_name);
		AssignVarchar(v_file, i, filename);
	}
	init->offset += n;
	duckdb_data_chunk_set_size(output, n);
}

void RegisterHealthkitWorkoutStatisticsFunction(duckdb_connection connection) {
	duckdb_table_function function = duckdb_create_table_function();
	duckdb_table_function_set_name(function, "healthkit_workout_statistics");
	duckdb_logical_type varchar_type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	duckdb_table_function_add_parameter(function, varchar_type);
	duckdb_destroy_logical_type(&varchar_type);
	duckdb_table_function_set_bind(function, StatsBind);
	duckdb_table_function_set_init(function, ScanInit);
	duckdb_table_function_set_function(function, StatsFunction);
	duckdb_register_table_function(connection, function);
	duckdb_destroy_table_function(&function);
}

/* -------------------------------------------------------------------------- */
/* healthkit_export_info                                                      */
/* -------------------------------------------------------------------------- */

typedef struct {
	ah_export_info row;
	int has_row;
	char *path;
	char *filename;
	int has_cda;
} info_bind_data;

static void DestroyInfoBind(void *ptr) {
	info_bind_data *b = (info_bind_data *)ptr;
	if (!b) {
		return;
	}
	free(b->path);
	free(b->filename);
	duckdb_free(b);
}

static void OnExportInfoCollect(const ah_export_info *row, void *userdata) {
	info_bind_data *b = (info_bind_data *)userdata;
	b->row = *row;
	b->has_row = 1;
}

static void InfoBind(duckdb_bind_info info) {
	duckdb_logical_type varchar_type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	duckdb_logical_type ts_type = duckdb_create_logical_type(DUCKDB_TYPE_TIMESTAMP_TZ);
	duckdb_logical_type bool_type = duckdb_create_logical_type(DUCKDB_TYPE_BOOLEAN);

	duckdb_bind_add_result_column(info, "export_date", ts_type);
	duckdb_bind_add_result_column(info, "source_path", varchar_type);
	duckdb_bind_add_result_column(info, "filename", varchar_type);
	duckdb_bind_add_result_column(info, "has_cda", bool_type);

	duckdb_destroy_logical_type(&varchar_type);
	duckdb_destroy_logical_type(&ts_type);
	duckdb_destroy_logical_type(&bool_type);

	char *path = BindPathOrError(info, "healthkit_export_info");
	if (!path) {
		return;
	}
	info_bind_data *bind = (info_bind_data *)duckdb_malloc(sizeof(info_bind_data));
	if (!bind) {
		free(path);
		duckdb_bind_set_error(info, "out of memory");
		return;
	}
	memset(bind, 0, sizeof(*bind));
	bind->path = path;

	/* Detect CDA via inventory when path is a zip. */
	ah_zip_member *members = NULL;
	size_t mcount = 0;
	char err[256];
	err[0] = '\0';
	if (ah_zip_list_members(bind->path, &members, &mcount, err, sizeof(err)) == 0) {
		for (size_t i = 0; i < mcount; i++) {
			if (strcmp(members[i].kind, "cda") == 0) {
				bind->has_cda = 1;
				break;
			}
		}
		free(members);
	}

	ah_xml_source *src = ah_xml_source_open(bind->path, err, sizeof(err));
	if (!src) {
		char msg[320];
		snprintf(msg, sizeof(msg), "healthkit_export_info: cannot open '%s': %s", bind->path, err[0] ? err : "error");
		DestroyInfoBind(bind);
		duckdb_bind_set_error(info, msg);
		return;
	}
	bind->filename = strdup(ah_xml_source_filename(src));
	ah_parse_callbacks cb = {
	    .on_record = OnRecordIgnore,
	    .on_workout = OnWorkoutIgnore,
	    .on_activity_summary = OnSummaryIgnore,
	    .on_workout_route = OnRouteIgnore,
	    .on_export_info = OnExportInfoCollect,
	    .userdata = bind,
	};
	ah_parse_stats stats;
	memset(&stats, 0, sizeof(stats));
	int rc = ah_parse_xml_filep(ah_xml_source_file(src), &cb, &stats);
	ah_xml_source_close(src);
	if (rc != 0) {
		DestroyInfoBind(bind);
		duckdb_bind_set_error(info, "healthkit_export_info: parse failed");
		return;
	}
	if (!bind->has_row) {
		/* Still emit one row with null export_date. */
		bind->has_row = 1;
		bind->row.export_date[0] = '\0';
	}
	duckdb_bind_set_bind_data(info, bind, DestroyInfoBind);
	duckdb_bind_set_cardinality(info, 1, true);
}

static void InfoFunction(duckdb_function_info info, duckdb_data_chunk output) {
	info_bind_data *bind = (info_bind_data *)duckdb_function_get_bind_data(info);
	scan_init_data *init = (scan_init_data *)duckdb_function_get_init_data(info);
	if (!bind || !init || !bind->has_row || init->offset >= 1) {
		duckdb_data_chunk_set_size(output, 0);
		return;
	}
	duckdb_vector v_date = duckdb_data_chunk_get_vector(output, 0);
	duckdb_vector v_path = duckdb_data_chunk_get_vector(output, 1);
	duckdb_vector v_file = duckdb_data_chunk_get_vector(output, 2);
	duckdb_vector v_cda = duckdb_data_chunk_get_vector(output, 3);

	duckdb_vector_ensure_validity_writable(v_date);
	uint64_t *date_v = duckdb_vector_get_validity(v_date);
	AssignTimestamp(v_date, date_v, 0, bind->row.export_date);
	AssignVarchar(v_path, 0, bind->path ? bind->path : "");
	AssignVarchar(v_file, 0, bind->filename ? bind->filename : "");
	bool *cda_data = (bool *)duckdb_vector_get_data(v_cda);
	cda_data[0] = bind->has_cda ? true : false;

	init->offset = 1;
	duckdb_data_chunk_set_size(output, 1);
}

void RegisterHealthkitExportInfoFunction(duckdb_connection connection) {
	duckdb_table_function function = duckdb_create_table_function();
	duckdb_table_function_set_name(function, "healthkit_export_info");
	duckdb_logical_type varchar_type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	duckdb_table_function_add_parameter(function, varchar_type);
	duckdb_destroy_logical_type(&varchar_type);
	duckdb_table_function_set_bind(function, InfoBind);
	duckdb_table_function_set_init(function, ScanInit);
	duckdb_table_function_set_function(function, InfoFunction);
	duckdb_register_table_function(connection, function);
	duckdb_destroy_table_function(&function);
}

/* -------------------------------------------------------------------------- */
/* healthkit_me                                                               */
/* -------------------------------------------------------------------------- */

typedef struct {
	ah_me row;
	int has_row;
	char *path;
	char *filename;
} me_bind_data;

static void DestroyMeBind(void *ptr) {
	me_bind_data *b = (me_bind_data *)ptr;
	if (!b) {
		return;
	}
	free(b->path);
	free(b->filename);
	duckdb_free(b);
}

static void OnMeCollect(const ah_me *row, void *userdata) {
	me_bind_data *b = (me_bind_data *)userdata;
	b->row = *row;
	b->has_row = 1;
}

static void MeBind(duckdb_bind_info info) {
	duckdb_logical_type varchar_type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	duckdb_bind_add_result_column(info, "date_of_birth", varchar_type);
	duckdb_bind_add_result_column(info, "biological_sex", varchar_type);
	duckdb_bind_add_result_column(info, "blood_type", varchar_type);
	duckdb_bind_add_result_column(info, "fitzpatrick_skin_type", varchar_type);
	duckdb_bind_add_result_column(info, "filename", varchar_type);
	duckdb_destroy_logical_type(&varchar_type);

	char *path = BindPathOrError(info, "healthkit_me");
	if (!path) {
		return;
	}
	me_bind_data *bind = (me_bind_data *)duckdb_malloc(sizeof(me_bind_data));
	if (!bind) {
		free(path);
		duckdb_bind_set_error(info, "out of memory");
		return;
	}
	memset(bind, 0, sizeof(*bind));
	bind->path = path;

	char err[256];
	err[0] = '\0';
	ah_xml_source *src = ah_xml_source_open(bind->path, err, sizeof(err));
	if (!src) {
		char msg[320];
		snprintf(msg, sizeof(msg), "healthkit_me: cannot open '%s': %s", bind->path, err[0] ? err : "error");
		DestroyMeBind(bind);
		duckdb_bind_set_error(info, msg);
		return;
	}
	bind->filename = strdup(ah_xml_source_filename(src));
	ah_parse_callbacks cb = {
	    .on_record = OnRecordIgnore,
	    .on_workout = OnWorkoutIgnore,
	    .on_activity_summary = OnSummaryIgnore,
	    .on_workout_route = OnRouteIgnore,
	    .on_me = OnMeCollect,
	    .userdata = bind,
	};
	ah_parse_stats stats;
	memset(&stats, 0, sizeof(stats));
	int rc = ah_parse_xml_filep(ah_xml_source_file(src), &cb, &stats);
	ah_xml_source_close(src);
	if (rc != 0) {
		DestroyMeBind(bind);
		duckdb_bind_set_error(info, "healthkit_me: parse failed");
		return;
	}
	if (!bind->has_row) {
		bind->has_row = 1;
		memset(&bind->row, 0, sizeof(bind->row));
	}
	duckdb_bind_set_bind_data(info, bind, DestroyMeBind);
	duckdb_bind_set_cardinality(info, 1, true);
}

static void MeFunction(duckdb_function_info info, duckdb_data_chunk output) {
	me_bind_data *bind = (me_bind_data *)duckdb_function_get_bind_data(info);
	scan_init_data *init = (scan_init_data *)duckdb_function_get_init_data(info);
	if (!bind || !init || !bind->has_row || init->offset >= 1) {
		duckdb_data_chunk_set_size(output, 0);
		return;
	}
	AssignVarchar(duckdb_data_chunk_get_vector(output, 0), 0, bind->row.date_of_birth);
	AssignVarchar(duckdb_data_chunk_get_vector(output, 1), 0, bind->row.biological_sex);
	AssignVarchar(duckdb_data_chunk_get_vector(output, 2), 0, bind->row.blood_type);
	AssignVarchar(duckdb_data_chunk_get_vector(output, 3), 0, bind->row.fitzpatrick_skin_type);
	AssignVarchar(duckdb_data_chunk_get_vector(output, 4), 0, bind->filename ? bind->filename : "");
	init->offset = 1;
	duckdb_data_chunk_set_size(output, 1);
}

void RegisterHealthkitMeFunction(duckdb_connection connection) {
	duckdb_table_function function = duckdb_create_table_function();
	duckdb_table_function_set_name(function, "healthkit_me");
	duckdb_logical_type varchar_type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	duckdb_table_function_add_parameter(function, varchar_type);
	duckdb_destroy_logical_type(&varchar_type);
	duckdb_table_function_set_bind(function, MeBind);
	duckdb_table_function_set_init(function, ScanInit);
	duckdb_table_function_set_function(function, MeFunction);
	duckdb_register_table_function(connection, function);
	duckdb_destroy_table_function(&function);
}

/* -------------------------------------------------------------------------- */
/* healthkit_zip_inventory                                                    */
/* -------------------------------------------------------------------------- */

typedef struct {
	ah_zip_member *rows;
	size_t count;
	char *path;
} inv_bind_data;

static void DestroyInvBind(void *ptr) {
	inv_bind_data *b = (inv_bind_data *)ptr;
	if (!b) {
		return;
	}
	free(b->rows);
	free(b->path);
	duckdb_free(b);
}

static void InvBind(duckdb_bind_info info) {
	duckdb_logical_type varchar_type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	duckdb_logical_type ubig_type = duckdb_create_logical_type(DUCKDB_TYPE_UBIGINT);

	duckdb_bind_add_result_column(info, "member_path", varchar_type);
	duckdb_bind_add_result_column(info, "uncompressed_size", ubig_type);
	duckdb_bind_add_result_column(info, "compressed_size", ubig_type);
	duckdb_bind_add_result_column(info, "kind", varchar_type);

	duckdb_destroy_logical_type(&varchar_type);
	duckdb_destroy_logical_type(&ubig_type);

	char *path = BindPathOrError(info, "healthkit_zip_inventory");
	if (!path) {
		return;
	}
	inv_bind_data *bind = (inv_bind_data *)duckdb_malloc(sizeof(inv_bind_data));
	if (!bind) {
		free(path);
		duckdb_bind_set_error(info, "out of memory");
		return;
	}
	memset(bind, 0, sizeof(*bind));
	bind->path = path;

	char err[256];
	err[0] = '\0';
	if (ah_zip_list_members(bind->path, &bind->rows, &bind->count, err, sizeof(err)) != 0) {
		char msg[320];
		snprintf(msg, sizeof(msg), "healthkit_zip_inventory: %s", err[0] ? err : "list failed");
		DestroyInvBind(bind);
		duckdb_bind_set_error(info, msg);
		return;
	}
	duckdb_bind_set_bind_data(info, bind, DestroyInvBind);
	duckdb_bind_set_cardinality(info, (idx_t)bind->count, true);
}

static void InvFunction(duckdb_function_info info, duckdb_data_chunk output) {
	inv_bind_data *bind = (inv_bind_data *)duckdb_function_get_bind_data(info);
	scan_init_data *init = (scan_init_data *)duckdb_function_get_init_data(info);
	if (!bind || !init || init->offset >= bind->count) {
		duckdb_data_chunk_set_size(output, 0);
		return;
	}
	const idx_t vector_size = duckdb_vector_size();
	size_t remaining = bind->count - init->offset;
	idx_t n = remaining > (size_t)vector_size ? vector_size : (idx_t)remaining;

	duckdb_vector v_path = duckdb_data_chunk_get_vector(output, 0);
	duckdb_vector v_uncomp = duckdb_data_chunk_get_vector(output, 1);
	duckdb_vector v_comp = duckdb_data_chunk_get_vector(output, 2);
	duckdb_vector v_kind = duckdb_data_chunk_get_vector(output, 3);
	uint64_t *uncomp = (uint64_t *)duckdb_vector_get_data(v_uncomp);
	uint64_t *comp = (uint64_t *)duckdb_vector_get_data(v_comp);

	for (idx_t i = 0; i < n; i++) {
		const ah_zip_member *row = &bind->rows[init->offset + i];
		AssignVarchar(v_path, i, row->path);
		uncomp[i] = row->uncompressed_size;
		comp[i] = row->compressed_size;
		AssignVarchar(v_kind, i, row->kind);
	}
	init->offset += n;
	duckdb_data_chunk_set_size(output, n);
}

void RegisterHealthkitZipInventoryFunction(duckdb_connection connection) {
	duckdb_table_function function = duckdb_create_table_function();
	duckdb_table_function_set_name(function, "healthkit_zip_inventory");
	duckdb_logical_type varchar_type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	duckdb_table_function_add_parameter(function, varchar_type);
	duckdb_destroy_logical_type(&varchar_type);
	duckdb_table_function_set_bind(function, InvBind);
	duckdb_table_function_set_init(function, ScanInit);
	duckdb_table_function_set_function(function, InvFunction);
	duckdb_register_table_function(connection, function);
	duckdb_destroy_table_function(&function);
}

/* -------------------------------------------------------------------------- */
/* healthkit_correlations                                                     */
/* -------------------------------------------------------------------------- */

typedef struct {
	ah_correlation *rows;
	size_t count;
	size_t capacity;
	char *path;
	char *filename;
} corr_bind_data;

static void DestroyCorrBind(void *ptr) {
	corr_bind_data *b = (corr_bind_data *)ptr;
	if (!b) return;
	free(b->rows);
	free(b->path);
	free(b->filename);
	duckdb_free(b);
}

static void OnCorrelationCollect(const ah_correlation *row, void *userdata) {
	corr_bind_data *b = (corr_bind_data *)userdata;
	if (b->count >= b->capacity) {
		size_t ncap = b->capacity ? b->capacity * 2 : 8;
		ah_correlation *nr = (ah_correlation *)realloc(b->rows, ncap * sizeof(ah_correlation));
		if (!nr) return;
		b->rows = nr;
		b->capacity = ncap;
	}
	b->rows[b->count++] = *row;
}

static void CorrBind(duckdb_bind_info info) {
	duckdb_logical_type varchar_type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	duckdb_logical_type ts_type = duckdb_create_logical_type(DUCKDB_TYPE_TIMESTAMP_TZ);
	duckdb_bind_add_result_column(info, "type", varchar_type);
	duckdb_bind_add_result_column(info, "type_short", varchar_type);
	duckdb_bind_add_result_column(info, "start_date", ts_type);
	duckdb_bind_add_result_column(info, "end_date", ts_type);
	duckdb_bind_add_result_column(info, "creation_date", ts_type);
	duckdb_bind_add_result_column(info, "source_name", varchar_type);
	duckdb_bind_add_result_column(info, "source_version", varchar_type);
	duckdb_bind_add_result_column(info, "filename", varchar_type);
	duckdb_destroy_logical_type(&varchar_type);
	duckdb_destroy_logical_type(&ts_type);

	char *path = BindPathOrError(info, "healthkit_correlations");
	if (!path) return;
	corr_bind_data *bind = (corr_bind_data *)duckdb_malloc(sizeof(corr_bind_data));
	if (!bind) { free(path); duckdb_bind_set_error(info, "out of memory"); return; }
	memset(bind, 0, sizeof(*bind));
	bind->path = path;
	char err[256]; err[0] = '\0';
	ah_xml_source *src = ah_xml_source_open(bind->path, err, sizeof(err));
	if (!src) {
		char msg[320];
		snprintf(msg, sizeof(msg), "healthkit_correlations: cannot open '%s': %s", bind->path, err[0] ? err : "error");
		DestroyCorrBind(bind);
		duckdb_bind_set_error(info, msg);
		return;
	}
	bind->filename = strdup(ah_xml_source_filename(src));
	ah_parse_callbacks cb = {
	    .on_record = OnRecordIgnore,
	    .on_workout = OnWorkoutIgnore,
	    .on_activity_summary = OnSummaryIgnore,
	    .on_workout_route = OnRouteIgnore,
	    .on_correlation = OnCorrelationCollect,
	    .userdata = bind,
	};
	ah_parse_stats stats; memset(&stats, 0, sizeof(stats));
	int rc = ah_parse_xml_filep(ah_xml_source_file(src), &cb, &stats);
	ah_xml_source_close(src);
	if (rc != 0) { DestroyCorrBind(bind); duckdb_bind_set_error(info, "healthkit_correlations: parse failed"); return; }
	duckdb_bind_set_bind_data(info, bind, DestroyCorrBind);
	duckdb_bind_set_cardinality(info, (idx_t)bind->count, true);
}

static void CorrFunction(duckdb_function_info info, duckdb_data_chunk output) {
	corr_bind_data *bind = (corr_bind_data *)duckdb_function_get_bind_data(info);
	scan_init_data *init = (scan_init_data *)duckdb_function_get_init_data(info);
	if (!bind || !init || init->offset >= bind->count) { duckdb_data_chunk_set_size(output, 0); return; }
	const idx_t vector_size = duckdb_vector_size();
	size_t remaining = bind->count - init->offset;
	idx_t n = remaining > (size_t)vector_size ? vector_size : (idx_t)remaining;
	duckdb_vector v0 = duckdb_data_chunk_get_vector(output, 0);
	duckdb_vector v1 = duckdb_data_chunk_get_vector(output, 1);
	duckdb_vector v2 = duckdb_data_chunk_get_vector(output, 2);
	duckdb_vector v3 = duckdb_data_chunk_get_vector(output, 3);
	duckdb_vector v4 = duckdb_data_chunk_get_vector(output, 4);
	duckdb_vector v5 = duckdb_data_chunk_get_vector(output, 5);
	duckdb_vector v6 = duckdb_data_chunk_get_vector(output, 6);
	duckdb_vector v7 = duckdb_data_chunk_get_vector(output, 7);
	duckdb_vector_ensure_validity_writable(v2);
	duckdb_vector_ensure_validity_writable(v3);
	duckdb_vector_ensure_validity_writable(v4);
	uint64_t *s_v = duckdb_vector_get_validity(v2);
	uint64_t *e_v = duckdb_vector_get_validity(v3);
	uint64_t *c_v = duckdb_vector_get_validity(v4);
	const char *filename = bind->filename ? bind->filename : "";
	for (idx_t i = 0; i < n; i++) {
		const ah_correlation *row = &bind->rows[init->offset + i];
		AssignVarchar(v0, i, row->type);
		AssignVarchar(v1, i, row->type_short);
		AssignTimestamp(v2, s_v, i, row->start_date);
		AssignTimestamp(v3, e_v, i, row->end_date);
		AssignTimestamp(v4, c_v, i, row->creation_date);
		AssignVarchar(v5, i, row->source_name);
		AssignVarchar(v6, i, row->source_version);
		AssignVarchar(v7, i, filename);
	}
	init->offset += n;
	duckdb_data_chunk_set_size(output, n);
}

void RegisterHealthkitCorrelationsFunction(duckdb_connection connection) {
	duckdb_table_function function = duckdb_create_table_function();
	duckdb_table_function_set_name(function, "healthkit_correlations");
	duckdb_logical_type varchar_type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	duckdb_table_function_add_parameter(function, varchar_type);
	duckdb_destroy_logical_type(&varchar_type);
	duckdb_table_function_set_bind(function, CorrBind);
	duckdb_table_function_set_init(function, ScanInit);
	duckdb_table_function_set_function(function, CorrFunction);
	duckdb_register_table_function(connection, function);
	duckdb_destroy_table_function(&function);
}

/* -------------------------------------------------------------------------- */
/* healthkit_correlation_members                                              */
/* -------------------------------------------------------------------------- */

typedef struct {
	ah_correlation_member *rows;
	size_t count;
	size_t capacity;
	char *path;
	char *filename;
} cmem_bind_data;

static void DestroyCmemBind(void *ptr) {
	cmem_bind_data *b = (cmem_bind_data *)ptr;
	if (!b) return;
	free(b->rows);
	free(b->path);
	free(b->filename);
	duckdb_free(b);
}

static void OnCmemCollect(const ah_correlation_member *row, void *userdata) {
	cmem_bind_data *b = (cmem_bind_data *)userdata;
	if (b->count >= b->capacity) {
		size_t ncap = b->capacity ? b->capacity * 2 : 8;
		ah_correlation_member *nr = (ah_correlation_member *)realloc(b->rows, ncap * sizeof(ah_correlation_member));
		if (!nr) return;
		b->rows = nr;
		b->capacity = ncap;
	}
	b->rows[b->count++] = *row;
}

static void CmemBind(duckdb_bind_info info) {
	duckdb_logical_type varchar_type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	duckdb_logical_type double_type = duckdb_create_logical_type(DUCKDB_TYPE_DOUBLE);
	duckdb_logical_type ts_type = duckdb_create_logical_type(DUCKDB_TYPE_TIMESTAMP_TZ);
	duckdb_bind_add_result_column(info, "correlation_type", varchar_type);
	duckdb_bind_add_result_column(info, "correlation_type_short", varchar_type);
	duckdb_bind_add_result_column(info, "correlation_start_date", ts_type);
	duckdb_bind_add_result_column(info, "correlation_end_date", ts_type);
	duckdb_bind_add_result_column(info, "correlation_source_name", varchar_type);
	duckdb_bind_add_result_column(info, "type", varchar_type);
	duckdb_bind_add_result_column(info, "type_short", varchar_type);
	duckdb_bind_add_result_column(info, "unit", varchar_type);
	duckdb_bind_add_result_column(info, "value", double_type);
	duckdb_bind_add_result_column(info, "value_text", varchar_type);
	duckdb_bind_add_result_column(info, "start_date", ts_type);
	duckdb_bind_add_result_column(info, "end_date", ts_type);
	duckdb_bind_add_result_column(info, "source_name", varchar_type);
	duckdb_bind_add_result_column(info, "filename", varchar_type);
	duckdb_destroy_logical_type(&varchar_type);
	duckdb_destroy_logical_type(&double_type);
	duckdb_destroy_logical_type(&ts_type);

	char *path = BindPathOrError(info, "healthkit_correlation_members");
	if (!path) return;
	cmem_bind_data *bind = (cmem_bind_data *)duckdb_malloc(sizeof(cmem_bind_data));
	if (!bind) { free(path); duckdb_bind_set_error(info, "out of memory"); return; }
	memset(bind, 0, sizeof(*bind));
	bind->path = path;
	char err[256]; err[0] = '\0';
	ah_xml_source *src = ah_xml_source_open(bind->path, err, sizeof(err));
	if (!src) {
		char msg[320];
		snprintf(msg, sizeof(msg), "healthkit_correlation_members: cannot open '%s': %s", bind->path, err[0] ? err : "error");
		DestroyCmemBind(bind);
		duckdb_bind_set_error(info, msg);
		return;
	}
	bind->filename = strdup(ah_xml_source_filename(src));
	ah_parse_callbacks cb = {
	    .on_record = OnRecordIgnore,
	    .on_workout = OnWorkoutIgnore,
	    .on_activity_summary = OnSummaryIgnore,
	    .on_workout_route = OnRouteIgnore,
	    .on_correlation_member = OnCmemCollect,
	    .userdata = bind,
	};
	ah_parse_stats stats; memset(&stats, 0, sizeof(stats));
	int rc = ah_parse_xml_filep(ah_xml_source_file(src), &cb, &stats);
	ah_xml_source_close(src);
	if (rc != 0) { DestroyCmemBind(bind); duckdb_bind_set_error(info, "healthkit_correlation_members: parse failed"); return; }
	duckdb_bind_set_bind_data(info, bind, DestroyCmemBind);
	duckdb_bind_set_cardinality(info, (idx_t)bind->count, true);
}

static void CmemFunction(duckdb_function_info info, duckdb_data_chunk output) {
	cmem_bind_data *bind = (cmem_bind_data *)duckdb_function_get_bind_data(info);
	scan_init_data *init = (scan_init_data *)duckdb_function_get_init_data(info);
	if (!bind || !init || init->offset >= bind->count) { duckdb_data_chunk_set_size(output, 0); return; }
	const idx_t vector_size = duckdb_vector_size();
	size_t remaining = bind->count - init->offset;
	idx_t n = remaining > (size_t)vector_size ? vector_size : (idx_t)remaining;
	duckdb_vector cols[14];
	for (int c = 0; c < 14; c++) cols[c] = duckdb_data_chunk_get_vector(output, (idx_t)c);
	duckdb_vector_ensure_validity_writable(cols[2]);
	duckdb_vector_ensure_validity_writable(cols[3]);
	duckdb_vector_ensure_validity_writable(cols[8]);
	duckdb_vector_ensure_validity_writable(cols[10]);
	duckdb_vector_ensure_validity_writable(cols[11]);
	uint64_t *cs_v = duckdb_vector_get_validity(cols[2]);
	uint64_t *ce_v = duckdb_vector_get_validity(cols[3]);
	uint64_t *val_v = duckdb_vector_get_validity(cols[8]);
	uint64_t *ms_v = duckdb_vector_get_validity(cols[10]);
	uint64_t *me_v = duckdb_vector_get_validity(cols[11]);
	double *val_data = (double *)duckdb_vector_get_data(cols[8]);
	const char *filename = bind->filename ? bind->filename : "";
	for (idx_t i = 0; i < n; i++) {
		const ah_correlation_member *row = &bind->rows[init->offset + i];
		AssignVarchar(cols[0], i, row->correlation_type);
		AssignVarchar(cols[1], i, row->correlation_type_short);
		AssignTimestamp(cols[2], cs_v, i, row->correlation_start_date);
		AssignTimestamp(cols[3], ce_v, i, row->correlation_end_date);
		AssignVarchar(cols[4], i, row->correlation_source_name);
		AssignVarchar(cols[5], i, row->member.type);
		AssignVarchar(cols[6], i, row->member.type_short);
		AssignVarchar(cols[7], i, row->member.unit);
		if (row->member.has_value) val_data[i] = row->member.value;
		else duckdb_validity_set_row_invalid(val_v, i);
		AssignVarchar(cols[9], i, row->member.value_text);
		AssignTimestamp(cols[10], ms_v, i, row->member.start_date);
		AssignTimestamp(cols[11], me_v, i, row->member.end_date);
		AssignVarchar(cols[12], i, row->member.source_name);
		AssignVarchar(cols[13], i, filename);
	}
	init->offset += n;
	duckdb_data_chunk_set_size(output, n);
}

void RegisterHealthkitCorrelationMembersFunction(duckdb_connection connection) {
	duckdb_table_function function = duckdb_create_table_function();
	duckdb_table_function_set_name(function, "healthkit_correlation_members");
	duckdb_logical_type varchar_type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	duckdb_table_function_add_parameter(function, varchar_type);
	duckdb_destroy_logical_type(&varchar_type);
	duckdb_table_function_set_bind(function, CmemBind);
	duckdb_table_function_set_init(function, ScanInit);
	duckdb_table_function_set_function(function, CmemFunction);
	duckdb_register_table_function(connection, function);
	duckdb_destroy_table_function(&function);
}

/* -------------------------------------------------------------------------- */
/* healthkit_metadata                                                         */
/* -------------------------------------------------------------------------- */

typedef struct {
	ah_metadata_entry *rows;
	size_t count;
	size_t capacity;
	char *path;
	char *filename;
} meta_bind_data;

static void DestroyMetaBind(void *ptr) {
	meta_bind_data *b = (meta_bind_data *)ptr;
	if (!b) return;
	free(b->rows);
	free(b->path);
	free(b->filename);
	duckdb_free(b);
}

static void OnMetaCollect(const ah_metadata_entry *row, void *userdata) {
	meta_bind_data *b = (meta_bind_data *)userdata;
	if (b->count >= b->capacity) {
		size_t ncap = b->capacity ? b->capacity * 2 : 32;
		ah_metadata_entry *nr = (ah_metadata_entry *)realloc(b->rows, ncap * sizeof(ah_metadata_entry));
		if (!nr) return;
		b->rows = nr;
		b->capacity = ncap;
	}
	b->rows[b->count++] = *row;
}

static void MetaBind(duckdb_bind_info info) {
	duckdb_logical_type varchar_type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	duckdb_logical_type ts_type = duckdb_create_logical_type(DUCKDB_TYPE_TIMESTAMP_TZ);
	duckdb_bind_add_result_column(info, "parent_kind", varchar_type);
	duckdb_bind_add_result_column(info, "parent_type", varchar_type);
	duckdb_bind_add_result_column(info, "parent_type_short", varchar_type);
	duckdb_bind_add_result_column(info, "parent_start_date", ts_type);
	duckdb_bind_add_result_column(info, "parent_end_date", ts_type);
	duckdb_bind_add_result_column(info, "parent_source_name", varchar_type);
	duckdb_bind_add_result_column(info, "key", varchar_type);
	duckdb_bind_add_result_column(info, "value", varchar_type);
	duckdb_bind_add_result_column(info, "filename", varchar_type);
	duckdb_destroy_logical_type(&varchar_type);
	duckdb_destroy_logical_type(&ts_type);

	char *path = BindPathOrError(info, "healthkit_metadata");
	if (!path) return;
	meta_bind_data *bind = (meta_bind_data *)duckdb_malloc(sizeof(meta_bind_data));
	if (!bind) { free(path); duckdb_bind_set_error(info, "out of memory"); return; }
	memset(bind, 0, sizeof(*bind));
	bind->path = path;
	char err[256]; err[0] = '\0';
	ah_xml_source *src = ah_xml_source_open(bind->path, err, sizeof(err));
	if (!src) {
		char msg[320];
		snprintf(msg, sizeof(msg), "healthkit_metadata: cannot open '%s': %s", bind->path, err[0] ? err : "error");
		DestroyMetaBind(bind);
		duckdb_bind_set_error(info, msg);
		return;
	}
	bind->filename = strdup(ah_xml_source_filename(src));
	ah_parse_callbacks cb = {
	    .on_record = OnRecordIgnore,
	    .on_workout = OnWorkoutIgnore,
	    .on_activity_summary = OnSummaryIgnore,
	    .on_workout_route = OnRouteIgnore,
	    .on_metadata_entry = OnMetaCollect,
	    .userdata = bind,
	};
	ah_parse_stats stats; memset(&stats, 0, sizeof(stats));
	int rc = ah_parse_xml_filep(ah_xml_source_file(src), &cb, &stats);
	ah_xml_source_close(src);
	if (rc != 0) { DestroyMetaBind(bind); duckdb_bind_set_error(info, "healthkit_metadata: parse failed"); return; }
	duckdb_bind_set_bind_data(info, bind, DestroyMetaBind);
	duckdb_bind_set_cardinality(info, (idx_t)bind->count, true);
}

static void MetaFunction(duckdb_function_info info, duckdb_data_chunk output) {
	meta_bind_data *bind = (meta_bind_data *)duckdb_function_get_bind_data(info);
	scan_init_data *init = (scan_init_data *)duckdb_function_get_init_data(info);
	if (!bind || !init || init->offset >= bind->count) { duckdb_data_chunk_set_size(output, 0); return; }
	const idx_t vector_size = duckdb_vector_size();
	size_t remaining = bind->count - init->offset;
	idx_t n = remaining > (size_t)vector_size ? vector_size : (idx_t)remaining;
	duckdb_vector cols[9];
	for (int c = 0; c < 9; c++) cols[c] = duckdb_data_chunk_get_vector(output, (idx_t)c);
	duckdb_vector_ensure_validity_writable(cols[3]);
	duckdb_vector_ensure_validity_writable(cols[4]);
	uint64_t *ps_v = duckdb_vector_get_validity(cols[3]);
	uint64_t *pe_v = duckdb_vector_get_validity(cols[4]);
	const char *filename = bind->filename ? bind->filename : "";
	for (idx_t i = 0; i < n; i++) {
		const ah_metadata_entry *row = &bind->rows[init->offset + i];
		AssignVarchar(cols[0], i, row->parent_kind);
		AssignVarchar(cols[1], i, row->parent_type);
		AssignVarchar(cols[2], i, row->parent_type_short);
		AssignTimestamp(cols[3], ps_v, i, row->parent_start_date);
		AssignTimestamp(cols[4], pe_v, i, row->parent_end_date);
		AssignVarchar(cols[5], i, row->parent_source_name);
		AssignVarchar(cols[6], i, row->key);
		AssignVarchar(cols[7], i, row->value);
		AssignVarchar(cols[8], i, filename);
	}
	init->offset += n;
	duckdb_data_chunk_set_size(output, n);
}

void RegisterHealthkitMetadataFunction(duckdb_connection connection) {
	duckdb_table_function function = duckdb_create_table_function();
	duckdb_table_function_set_name(function, "healthkit_metadata");
	duckdb_logical_type varchar_type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	duckdb_table_function_add_parameter(function, varchar_type);
	duckdb_destroy_logical_type(&varchar_type);
	duckdb_table_function_set_bind(function, MetaBind);
	duckdb_table_function_set_init(function, ScanInit);
	duckdb_table_function_set_function(function, MetaFunction);
	duckdb_register_table_function(connection, function);
	duckdb_destroy_table_function(&function);
}
