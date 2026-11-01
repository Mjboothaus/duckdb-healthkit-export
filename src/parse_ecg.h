#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Privacy: never populate subject name or date of birth from ECG CSV headers. */

#define AH_ECG_PATH_MAX 512
#define AH_ECG_ATTR_MAX 256

typedef struct {
	char ecg_path[AH_ECG_PATH_MAX];
	char recorded_at[64]; /* as written in CSV */
	char classification[AH_ECG_ATTR_MAX];
	char symptoms[AH_ECG_ATTR_MAX];
	char software_version[64];
	char device[AH_ECG_ATTR_MAX];
	char sample_rate_text[64];
	double sample_rate_hz;
	bool has_sample_rate_hz;
	char lead[64];
	char unit[32];
	uint64_t n_samples;
} ah_ecg_recording;

typedef struct {
	char ecg_path[AH_ECG_PATH_MAX];
	uint64_t sample_index;
	double value_uv;
} ah_ecg_sample;

typedef struct {
	void (*on_recording)(const ah_ecg_recording *row, void *userdata);
	void (*on_sample)(const ah_ecg_sample *row, void *userdata);
	void *userdata;
} ah_ecg_callbacks;

/*
 * Parse one ECG CSV from FILE*.
 * Skips "Name" and "Date of Birth" header lines entirely.
 * Returns 0 on success.
 */
int ah_parse_ecg_filep(FILE *fp, const char *ecg_path, const ah_ecg_callbacks *cb, char *err, size_t err_len);

/*
 * Discover and parse all ECG CSVs under an export path (zip/dir/xml parent).
 * optional_filter_path: if non-NULL/non-empty, only members whose path contains this substring.
 */
int ah_parse_ecg_export(const char *export_path, const char *optional_filter_path, const ah_ecg_callbacks *cb,
                        char *err, size_t err_len);
