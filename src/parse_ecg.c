#include "parse_ecg.h"

#include "zip_source.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

static void set_err(char *err, size_t err_len, const char *msg) {
	if (err && err_len) {
		snprintf(err, err_len, "%s", msg ? msg : "error");
	}
}

static void trim_inplace(char *s) {
	if (!s) {
		return;
	}
	char *start = s;
	while (*start && isspace((unsigned char)*start)) {
		start++;
	}
	if (start != s) {
		memmove(s, start, strlen(start) + 1);
	}
	size_t n = strlen(s);
	while (n > 0 && isspace((unsigned char)s[n - 1])) {
		s[--n] = '\0';
	}
	/* strip surrounding quotes */
	if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
		s[n - 1] = '\0';
		memmove(s, s + 1, n - 1);
	}
}

static int split_csv_2(const char *line, char *key, size_t key_len, char *val, size_t val_len) {
	const char *comma = strchr(line, ',');
	if (!comma) {
		return 0;
	}
	size_t kn = (size_t)(comma - line);
	if (kn >= key_len) {
		kn = key_len - 1;
	}
	memcpy(key, line, kn);
	key[kn] = '\0';
	snprintf(val, val_len, "%s", comma + 1);
	trim_inplace(key);
	trim_inplace(val);
	return 1;
}

static int is_identity_key(const char *key) {
	/* Privacy: drop person-identifying header rows. */
	if (!key) {
		return 0;
	}
	if (strcasecmp(key, "Name") == 0) {
		return 1;
	}
	if (strcasecmp(key, "Date of Birth") == 0) {
		return 1;
	}
	if (strcasecmp(key, "Date of birth") == 0) {
		return 1;
	}
	return 0;
}

static double parse_hz(const char *text) {
	if (!text || !*text) {
		return 0.0;
	}
	/* e.g. "512 hertz" or "513.633 hertz" */
	char *end = NULL;
	double v = strtod(text, &end);
	if (end == text) {
		return 0.0;
	}
	return v;
}

int ah_parse_ecg_filep(FILE *fp, const char *ecg_path, const ah_ecg_callbacks *cb, char *err, size_t err_len) {
	if (!fp || !cb) {
		set_err(err, err_len, "invalid args");
		return 1;
	}
	ah_ecg_recording rec;
	memset(&rec, 0, sizeof(rec));
	if (ecg_path) {
		snprintf(rec.ecg_path, sizeof(rec.ecg_path), "%s", ecg_path);
	}

	char line[1024];
	int in_samples = 0;
	uint64_t idx = 0;

	while (fgets(line, sizeof(line), fp)) {
		/* strip CR/LF */
		size_t n = strlen(line);
		while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
			line[--n] = '\0';
		}
		if (!line[0]) {
			/* blank lines: once headers done, samples may start after unit */
			continue;
		}

		if (!in_samples) {
			char key[128];
			char val[AH_ECG_ATTR_MAX];
			if (split_csv_2(line, key, sizeof(key), val, sizeof(val))) {
				if (is_identity_key(key)) {
					continue;
				}
				if (strcasecmp(key, "Recorded Date") == 0) {
					snprintf(rec.recorded_at, sizeof(rec.recorded_at), "%s", val);
				} else if (strcasecmp(key, "Classification") == 0) {
					snprintf(rec.classification, sizeof(rec.classification), "%s", val);
				} else if (strcasecmp(key, "Symptoms") == 0) {
					snprintf(rec.symptoms, sizeof(rec.symptoms), "%s", val);
				} else if (strcasecmp(key, "Software Version") == 0) {
					snprintf(rec.software_version, sizeof(rec.software_version), "%s", val);
				} else if (strcasecmp(key, "Device") == 0) {
					snprintf(rec.device, sizeof(rec.device), "%s", val);
				} else if (strcasecmp(key, "Sample Rate") == 0) {
					snprintf(rec.sample_rate_text, sizeof(rec.sample_rate_text), "%s", val);
					double hz = parse_hz(val);
					if (hz > 0.0) {
						rec.sample_rate_hz = hz;
						rec.has_sample_rate_hz = true;
					}
				} else if (strcasecmp(key, "Lead") == 0) {
					snprintf(rec.lead, sizeof(rec.lead), "%s", val);
				} else if (strcasecmp(key, "Unit") == 0) {
					snprintf(rec.unit, sizeof(rec.unit), "%s", val);
					/* samples follow after unit header */
					in_samples = 1;
				}
				continue;
			}
			/* bare number before Unit? treat as sample once Unit seen */
			char *end = NULL;
			double v = strtod(line, &end);
			if (end != line && *end == '\0') {
				in_samples = 1;
				/* fall through to sample emit below */
			} else {
				continue;
			}
			if (in_samples && cb->on_sample) {
				ah_ecg_sample s;
				memset(&s, 0, sizeof(s));
				snprintf(s.ecg_path, sizeof(s.ecg_path), "%s", rec.ecg_path);
				s.sample_index = idx++;
				s.value_uv = v;
				cb->on_sample(&s, cb->userdata);
				rec.n_samples++;
			}
			continue;
		}

		/* sample lines */
		char *end = NULL;
		double v = strtod(line, &end);
		if (end == line) {
			continue;
		}
		if (cb->on_sample) {
			ah_ecg_sample s;
			memset(&s, 0, sizeof(s));
			snprintf(s.ecg_path, sizeof(s.ecg_path), "%s", rec.ecg_path);
			s.sample_index = idx++;
			s.value_uv = v;
			cb->on_sample(&s, cb->userdata);
		}
		rec.n_samples++;
	}

	if (ferror(fp)) {
		set_err(err, err_len, "ecg fread error");
		return 2;
	}
	if (cb->on_recording) {
		cb->on_recording(&rec, cb->userdata);
	}
	return 0;
}

int ah_parse_ecg_export(const char *export_path, const char *optional_filter_path, const ah_ecg_callbacks *cb,
                        char *err, size_t err_len) {
	if (!export_path || !cb) {
		set_err(err, err_len, "invalid args");
		return 1;
	}
	ah_zip_member *members = NULL;
	size_t count = 0;
	if (ah_zip_list_members(export_path, &members, &count, err, err_len) != 0) {
		/* plain xml path: no ECG members */
		if (err) {
			err[0] = '\0';
		}
		return 0;
	}
	int rc_all = 0;
	for (size_t i = 0; i < count; i++) {
		if (strcmp(members[i].kind, "ecg_csv") != 0) {
			continue;
		}
		if (optional_filter_path && optional_filter_path[0] &&
		    strstr(members[i].path, optional_filter_path) == NULL) {
			continue;
		}
		char logical[AH_ZIP_MEMBER_PATH_MAX + 2];
		if (members[i].path[0] != '/') {
			snprintf(logical, sizeof(logical), "/%s", members[i].path);
		} else {
			snprintf(logical, sizeof(logical), "%s", members[i].path);
		}
		/* strip leading apple_health_export if present for open_member */
		const char *log = logical;
		const char *p = strstr(logical, "/electrocardiograms/");
		if (p) {
			log = p;
		}
		char open_err[256];
		open_err[0] = '\0';
		ah_xml_source *src = ah_xml_source_open_member(export_path, log, open_err, sizeof(open_err));
		if (!src) {
			/* try full member path as logical */
			src = ah_xml_source_open_member(export_path, members[i].path, open_err, sizeof(open_err));
		}
		if (!src) {
			continue;
		}
		int rc = ah_parse_ecg_filep(ah_xml_source_file(src), members[i].path, cb, err, err_len);
		ah_xml_source_close(src);
		if (rc != 0) {
			rc_all = rc;
			break;
		}
	}
	free(members);
	return rc_all;
}
