#include "validation.h"

#include <stddef.h>
#include <string.h>

#include "cJSON.h"

/* ----- internal helpers (file-private) ----- */

static int ipman_is_digit(char ch) {
    return ch >= '0' && ch <= '9';
}

static int ipman_parse_fixed_uint(const char *value,
                                 size_t offset,
                                 size_t count) {
    int parsed = 0;
    for (size_t pos = 0; pos < count; ++pos) {
        char ch = value[offset + pos];
        if (!ipman_is_digit(ch)) return -1;
        parsed = parsed * 10 + (ch - '0');
    }
    return parsed;
}

static int ipman_days_in_month(int year, int month) {
    static const int days[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };
    if (month < 1 || month > 12) return 0;
    if (month == 2) {
        int leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
        return leap ? 29 : 28;
    }
    return days[month - 1];
}

static int ipman_is_valid_calendar_date(const char *value) {
    if (value[4] != '-' || value[7] != '-') return 0;
    int year = ipman_parse_fixed_uint(value, 0, 4);
    int month = ipman_parse_fixed_uint(value, 5, 2);
    int day = ipman_parse_fixed_uint(value, 8, 2);
    if (year < 0 || month < 0 || day < 0) return 0;
    return day >= 1 && day <= ipman_days_in_month(year, month);
}

static int ipman_is_valid_iso8601_datetime(const char *value, size_t len) {
    if (len < 19 || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':') {
        return 0;
    }
    int hour = ipman_parse_fixed_uint(value, 11, 2);
    int minute = ipman_parse_fixed_uint(value, 14, 2);
    int second = ipman_parse_fixed_uint(value, 17, 2);
    if (hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 ||
        second < 0 || second > 59) {
        return 0;
    }

    size_t pos = 19;
    if (pos < len && value[pos] == '.') {
        ++pos;
        size_t start = pos;
        while (pos < len && ipman_is_digit(value[pos])) ++pos;
        if (pos == start) return 0;
    }
    if (pos == len) return 1;
    if (value[pos] == 'Z') return pos + 1 == len;
    if ((value[pos] == '+' || value[pos] == '-') && pos + 6 == len &&
        value[pos + 3] == ':') {
        int tz_hour = ipman_parse_fixed_uint(value, pos + 1, 2);
        int tz_minute = ipman_parse_fixed_uint(value, pos + 4, 2);
        return tz_hour >= 0 && tz_hour <= 23 &&
               tz_minute >= 0 && tz_minute <= 59;
    }
    return 0;
}

static int ipman_is_valid_iso8601_date_text(const char *value) {
    if (value == NULL) return 1;
    size_t len = strlen(value);
    if (len < 10 || !ipman_is_valid_calendar_date(value)) return 0;
    if (len == 10) return 1;
    return ipman_is_valid_iso8601_datetime(value, len);
}

static int ipman_is_alnum(char ch) {
    return (ch >= 'a' && ch <= 'z') ||
           (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9');
}

static char ipman_tolower(char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch + ('a' - 'A');
    return ch;
}

/* ----- public API ----- */

int ipman_read_positive_id(cJSON *params, const char *field,
                          sqlite3_int64 *out,
                          ipman_error_code_t *err_code_out,
                          const char **err_msg_out) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(params, field);
    if (!cJSON_IsNumber(item) || item->valuedouble < 1.0) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "id must be a positive integer";
        return -1;
    }
    *out = (sqlite3_int64)item->valuedouble;
    return 0;
}

int ipman_validate_text_field(const char *field,
                             const char *value,
                             ipman_error_code_t *err_code_out,
                             const char **err_msg_out) {
    if (value == NULL) return 0;
    size_t len = strlen(value);
    if (strcmp(field, "title") == 0 && len > IPMAN_TITLE_MAX_BYTES) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "title must be at most 512 bytes";
        return -1;
    }
    if (strcmp(field, "summary") == 0 && len > IPMAN_SUMMARY_MAX_BYTES) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "summary must be at most 2048 bytes";
        return -1;
    }
    if (strcmp(field, "description") == 0 &&
        len > IPMAN_DESCRIPTION_MAX_BYTES) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "description must be at most 16384 bytes";
        return -1;
    }
    return 0;
}

int ipman_validate_date_field(const char *value,
                             ipman_error_code_t *err_code_out,
                             const char **err_msg_out) {
    if (!ipman_is_valid_iso8601_date_text(value)) {
        *err_code_out = IPMAN_ERR_VALIDATION_FAILED;
        *err_msg_out = "date fields must be ISO-8601 YYYY-MM-DD or datetime";
        return -1;
    }
    return 0;
}

void ipman_slugify(const char *title, char *out_slug, size_t max_len) {
    if (max_len == 0) return;
    out_slug[0] = '\0';
    if (title == NULL) return;

    size_t out_pos = 0;
    int last_was_dash = 1; /* start as 1 to trim leading dashes */

    for (size_t i = 0; title[i] != '\0'; ++i) {
        char ch = title[i];
        if (ipman_is_alnum(ch)) {
            if (out_pos < max_len - 1) {
                out_slug[out_pos++] = ipman_tolower(ch);
                last_was_dash = 0;
            }
        } else {
            if (!last_was_dash && out_pos < max_len - 1) {
                out_slug[out_pos++] = '-';
                last_was_dash = 1;
            }
        }
    }

    if (out_pos > 0 && out_slug[out_pos - 1] == '-') {
        out_pos--;
    }
    out_slug[out_pos] = '\0';
}
