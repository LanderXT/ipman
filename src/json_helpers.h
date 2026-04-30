#ifndef IPMAN_JSON_HELPERS_H
#define IPMAN_JSON_HELPERS_H

#include <sqlite3.h>

#include "cJSON.h"

/*
 * Shared cJSON serialization helpers for ipman entity row builders.
 *
 * v2 convention (since the format-version 1 → 2 bump): optional fields whose
 * underlying value is NULL are omitted from the JSON entirely, not emitted as
 * `"key":null`. These helpers are the single chokepoint that enforces that
 * convention for plan/phase/task/comment/event/closure rows and for the
 * canonical export. Workspace-context sentinels and response-envelope nulls
 * outside entity rows are intentionally not routed through here — they carry
 * "nothing here" semantics that absence wouldn't communicate.
 */

/* String column: no-op on NULL, otherwise add as string. */
void ipman_json_add_text_or_null(cJSON *obj, const char *key,
                                const unsigned char *value);

/* Integer column read from a sqlite3 statement: no-op when the column is
 * SQL NULL, otherwise add as a JSON number. */
void ipman_json_add_int64_or_null(cJSON *obj, const char *key,
                                 sqlite3_stmt *stmt, int col);

/* JSON-text column (events.details, events.old_value, events.new_value):
 * no-op on NULL, parse and embed when valid, fall back to the raw string
 * when the stored bytes aren't parseable JSON. */
void ipman_json_add_json_or_null(cJSON *obj, const char *key,
                                const unsigned char *value);

/* JSON-array column (plans.tags): emits an empty array on NULL so the field
 * is always a JSON array on the wire; falls back to the raw string when the
 * stored bytes aren't parseable as an array (debug aid for malformed data,
 * preserved from the historical read-side behavior). */
void ipman_json_add_tags(cJSON *obj, const char *key,
                        const unsigned char *value);

#endif
