#include "json_helpers.h"

void ipman_json_add_text_or_null(cJSON *obj, const char *key,
                                const unsigned char *value) {
    if (value == NULL) return;
    cJSON_AddStringToObject(obj, key, (const char *)value);
}

void ipman_json_add_int64_or_null(cJSON *obj, const char *key,
                                 sqlite3_stmt *stmt, int col) {
    if (sqlite3_column_type(stmt, col) == SQLITE_NULL) return;
    cJSON_AddNumberToObject(obj, key,
                            (double)sqlite3_column_int64(stmt, col));
}

void ipman_json_add_json_or_null(cJSON *obj, const char *key,
                                const unsigned char *value) {
    if (value == NULL) return;
    cJSON *parsed = cJSON_Parse((const char *)value);
    if (parsed == NULL) {
        /* Stored value isn't parseable JSON; surface as a string so the
         * round-trip is at least lossless rather than silently dropping
         * the column. Should not happen for fields ipman writes itself. */
        cJSON_AddStringToObject(obj, key, (const char *)value);
        return;
    }
    cJSON_AddItemToObject(obj, key, parsed);
}

void ipman_json_add_tags(cJSON *obj, const char *key,
                        const unsigned char *value) {
    if (value == NULL) {
        cJSON_AddItemToObject(obj, key, cJSON_CreateArray());
        return;
    }
    cJSON *parsed = cJSON_Parse((const char *)value);
    if (parsed == NULL) {
        /* Bad JSON in the tags column — surface as the raw string so the
         * problem is visible to the caller. */
        cJSON_AddStringToObject(obj, key, (const char *)value);
        return;
    }
    cJSON_AddItemToObject(obj, key, parsed);
}
