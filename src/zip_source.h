#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "parse_health.h"

/* Open a Health app export path as a stream of export.xml bytes.
 * path may be:
 *   - .zip containing a member ending in export.xml (e.g. apple_health_export/export.xml)
 *   - directory containing export.xml
 *   - export.xml (or any .xml file)
 *
 * No DuckDB headers. Uses zlib for DEFLATE members only.
 */

typedef struct ah_xml_source ah_xml_source;

/* Open source. On success returns non-NULL; caller must ah_xml_source_close. */
ah_xml_source *ah_xml_source_open(const char *path, char *err, size_t err_len);

/* FILE* positioned at start of export.xml content (may be a temp file). */
FILE *ah_xml_source_file(ah_xml_source *src);

/* Path used for the logical filename column (zip member or file path). */
const char *ah_xml_source_filename(const ah_xml_source *src);

void ah_xml_source_close(ah_xml_source *src);

/* Open a companion member relative to an export path.
 * export_path: .zip / dir / export.xml
 * logical_path: FileReference path, e.g. "/workout-routes/route_….gpx"
 * Resolves zip member *…/workout-routes/… or filesystem next to export.xml.
 */
ah_xml_source *ah_xml_source_open_member(const char *export_path, const char *logical_path, char *err,
                                         size_t err_len);

/* Convenience: open path, parse with callbacks, close. */
int ah_parse_health_path(const char *path, const ah_parse_callbacks *cb, ah_parse_stats *stats_out, char *err,
                         size_t err_len);

/* Zip / dir inventory (no full inflate of members). */
#define AH_ZIP_MEMBER_PATH_MAX 512

typedef struct {
	char path[AH_ZIP_MEMBER_PATH_MAX];
	uint64_t uncompressed_size;
	uint64_t compressed_size;
	char kind[32]; /* export_xml | gpx | ecg_csv | cda | other | directory */
} ah_zip_member;

/* On success: *out heap array of *count members (caller free(*out)). Returns 0. */
int ah_zip_list_members(const char *path, ah_zip_member **out, size_t *count, char *err, size_t err_len);
