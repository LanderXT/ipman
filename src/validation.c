/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#include "validation.h"

#include <stddef.h>
#include <stdint.h>
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

/* ----- UTF-8 decoder (Task #83) ------------------------------------------- */

/*
 * Decode one UTF-8 codepoint from s[0..len-1].
 * On success: writes codepoint to *cp_out, bytes consumed to *bytes_out,
 *             returns 0.
 * On invalid/overlong/surrogate/out-of-range: returns -1.
 * Rejects: overlong encodings, surrogates (U+D800..U+DFFF), cp > U+10FFFF,
 *          truncated sequences.
 */
static int ipman_utf8_decode_next(const unsigned char *s, size_t len,
                                  uint32_t *cp_out, size_t *bytes_out) {
    if (len == 0) return -1;

    unsigned char b0 = s[0];

    if (b0 < 0x80) {
        /* ASCII: single byte */
        *cp_out = b0;
        *bytes_out = 1;
        return 0;
    }

    size_t seq_len;
    uint32_t cp;

    if ((b0 & 0xE0) == 0xC0) {
        seq_len = 2;
        cp = b0 & 0x1F;
    } else if ((b0 & 0xF0) == 0xE0) {
        seq_len = 3;
        cp = b0 & 0x0F;
    } else if ((b0 & 0xF8) == 0xF0) {
        seq_len = 4;
        cp = b0 & 0x07;
    } else {
        /* Lone continuation byte or invalid lead byte */
        return -1;
    }

    if (len < seq_len) return -1; /* truncated */

    for (size_t i = 1; i < seq_len; ++i) {
        unsigned char cb = s[i];
        if ((cb & 0xC0) != 0x80) return -1; /* not a continuation byte */
        cp = (cp << 6) | (cb & 0x3F);
    }

    /* Reject overlong encodings */
    if (seq_len == 2 && cp < 0x0080) return -1;
    if (seq_len == 3 && cp < 0x0800) return -1;
    if (seq_len == 4 && cp < 0x10000) return -1;

    /* Reject surrogates */
    if (cp >= 0xD800 && cp <= 0xDFFF) return -1;

    /* Reject codepoints above U+10FFFF */
    if (cp > 0x10FFFF) return -1;

    *cp_out = cp;
    *bytes_out = seq_len;
    return 0;
}

/* ----- Transliteration table (Task #84) ------------------------------------ */

/*
 * Latin-1 Supplement (U+00C0..U+00FF) and Latin Extended-A (U+0100..U+017F).
 * 288 entries, indexed by (cp - 0x00C0).
 * Each entry is a NUL-terminated string of 1-2 ASCII lowercase chars,
 * or NULL meaning "no mapping → emit dash (collapsed)".
 *
 * The table covers both upper- and lowercase variants; ipman_slugify
 * lowercases mapped strings via ipman_tolower on each char.
 *
 * References: Unicode NamesList, common slug-generation conventions.
 */
static const char * const ipman_latin_translit[288] = {
    /* U+00C0 */ "a",   /* À  LATIN CAPITAL LETTER A WITH GRAVE */
    /* U+00C1 */ "a",   /* Á  LATIN CAPITAL LETTER A WITH ACUTE */
    /* U+00C2 */ "a",   /* Â  LATIN CAPITAL LETTER A WITH CIRCUMFLEX */
    /* U+00C3 */ "a",   /* Ã  LATIN CAPITAL LETTER A WITH TILDE */
    /* U+00C4 */ "a",   /* Ä  LATIN CAPITAL LETTER A WITH DIAERESIS */
    /* U+00C5 */ "a",   /* Å  LATIN CAPITAL LETTER A WITH RING ABOVE */
    /* U+00C6 */ "ae",  /* Æ  LATIN CAPITAL LETTER AE */
    /* U+00C7 */ "c",   /* Ç  LATIN CAPITAL LETTER C WITH CEDILLA */
    /* U+00C8 */ "e",   /* È  LATIN CAPITAL LETTER E WITH GRAVE */
    /* U+00C9 */ "e",   /* É  LATIN CAPITAL LETTER E WITH ACUTE */
    /* U+00CA */ "e",   /* Ê  LATIN CAPITAL LETTER E WITH CIRCUMFLEX */
    /* U+00CB */ "e",   /* Ë  LATIN CAPITAL LETTER E WITH DIAERESIS */
    /* U+00CC */ "i",   /* Ì  LATIN CAPITAL LETTER I WITH GRAVE */
    /* U+00CD */ "i",   /* Í  LATIN CAPITAL LETTER I WITH ACUTE */
    /* U+00CE */ "i",   /* Î  LATIN CAPITAL LETTER I WITH CIRCUMFLEX */
    /* U+00CF */ "i",   /* Ï  LATIN CAPITAL LETTER I WITH DIAERESIS */
    /* U+00D0 */ "d",   /* Ð  LATIN CAPITAL LETTER ETH */
    /* U+00D1 */ "n",   /* Ñ  LATIN CAPITAL LETTER N WITH TILDE */
    /* U+00D2 */ "o",   /* Ò  LATIN CAPITAL LETTER O WITH GRAVE */
    /* U+00D3 */ "o",   /* Ó  LATIN CAPITAL LETTER O WITH ACUTE */
    /* U+00D4 */ "o",   /* Ô  LATIN CAPITAL LETTER O WITH CIRCUMFLEX */
    /* U+00D5 */ "o",   /* Õ  LATIN CAPITAL LETTER O WITH TILDE */
    /* U+00D6 */ "o",   /* Ö  LATIN CAPITAL LETTER O WITH DIAERESIS */
    /* U+00D7 */ NULL,  /* ×  MULTIPLICATION SIGN */
    /* U+00D8 */ "o",   /* Ø  LATIN CAPITAL LETTER O WITH STROKE */
    /* U+00D9 */ "u",   /* Ù  LATIN CAPITAL LETTER U WITH GRAVE */
    /* U+00DA */ "u",   /* Ú  LATIN CAPITAL LETTER U WITH ACUTE */
    /* U+00DB */ "u",   /* Û  LATIN CAPITAL LETTER U WITH CIRCUMFLEX */
    /* U+00DC */ "u",   /* Ü  LATIN CAPITAL LETTER U WITH DIAERESIS */
    /* U+00DD */ "y",   /* Ý  LATIN CAPITAL LETTER Y WITH ACUTE */
    /* U+00DE */ "th",  /* Þ  LATIN CAPITAL LETTER THORN */
    /* U+00DF */ "ss",  /* ß  LATIN SMALL LETTER SHARP S */
    /* U+00E0 */ "a",   /* à  LATIN SMALL LETTER A WITH GRAVE */
    /* U+00E1 */ "a",   /* á  LATIN SMALL LETTER A WITH ACUTE */
    /* U+00E2 */ "a",   /* â  LATIN SMALL LETTER A WITH CIRCUMFLEX */
    /* U+00E3 */ "a",   /* ã  LATIN SMALL LETTER A WITH TILDE */
    /* U+00E4 */ "a",   /* ä  LATIN SMALL LETTER A WITH DIAERESIS */
    /* U+00E5 */ "a",   /* å  LATIN SMALL LETTER A WITH RING ABOVE */
    /* U+00E6 */ "ae",  /* æ  LATIN SMALL LETTER AE */
    /* U+00E7 */ "c",   /* ç  LATIN SMALL LETTER C WITH CEDILLA */
    /* U+00E8 */ "e",   /* è  LATIN SMALL LETTER E WITH GRAVE */
    /* U+00E9 */ "e",   /* é  LATIN SMALL LETTER E WITH ACUTE */
    /* U+00EA */ "e",   /* ê  LATIN SMALL LETTER E WITH CIRCUMFLEX */
    /* U+00EB */ "e",   /* ë  LATIN SMALL LETTER E WITH DIAERESIS */
    /* U+00EC */ "i",   /* ì  LATIN SMALL LETTER I WITH GRAVE */
    /* U+00ED */ "i",   /* í  LATIN SMALL LETTER I WITH ACUTE */
    /* U+00EE */ "i",   /* î  LATIN SMALL LETTER I WITH CIRCUMFLEX */
    /* U+00EF */ "i",   /* ï  LATIN SMALL LETTER I WITH DIAERESIS */
    /* U+00F0 */ "d",   /* ð  LATIN SMALL LETTER ETH */
    /* U+00F1 */ "n",   /* ñ  LATIN SMALL LETTER N WITH TILDE */
    /* U+00F2 */ "o",   /* ò  LATIN SMALL LETTER O WITH GRAVE */
    /* U+00F3 */ "o",   /* ó  LATIN SMALL LETTER O WITH ACUTE */
    /* U+00F4 */ "o",   /* ô  LATIN SMALL LETTER O WITH CIRCUMFLEX */
    /* U+00F5 */ "o",   /* õ  LATIN SMALL LETTER O WITH TILDE */
    /* U+00F6 */ "o",   /* ö  LATIN SMALL LETTER O WITH DIAERESIS */
    /* U+00F7 */ NULL,  /* ÷  DIVISION SIGN */
    /* U+00F8 */ "o",   /* ø  LATIN SMALL LETTER O WITH STROKE */
    /* U+00F9 */ "u",   /* ù  LATIN SMALL LETTER U WITH GRAVE */
    /* U+00FA */ "u",   /* ú  LATIN SMALL LETTER U WITH ACUTE */
    /* U+00FB */ "u",   /* û  LATIN SMALL LETTER U WITH CIRCUMFLEX */
    /* U+00FC */ "u",   /* ü  LATIN SMALL LETTER U WITH DIAERESIS */
    /* U+00FD */ "y",   /* ý  LATIN SMALL LETTER Y WITH ACUTE */
    /* U+00FE */ "th",  /* þ  LATIN SMALL LETTER THORN */
    /* U+00FF */ "y",   /* ÿ  LATIN SMALL LETTER Y WITH DIAERESIS */

    /* --- Latin Extended-A: U+0100..U+017F (128 entries) --- */

    /* U+0100 */ "a",   /* Ā  LATIN CAPITAL LETTER A WITH MACRON */
    /* U+0101 */ "a",   /* ā  LATIN SMALL LETTER A WITH MACRON */
    /* U+0102 */ "a",   /* Ă  LATIN CAPITAL LETTER A WITH BREVE */
    /* U+0103 */ "a",   /* ă  LATIN SMALL LETTER A WITH BREVE */
    /* U+0104 */ "a",   /* Ą  LATIN CAPITAL LETTER A WITH OGONEK */
    /* U+0105 */ "a",   /* ą  LATIN SMALL LETTER A WITH OGONEK */
    /* U+0106 */ "c",   /* Ć  LATIN CAPITAL LETTER C WITH ACUTE */
    /* U+0107 */ "c",   /* ć  LATIN SMALL LETTER C WITH ACUTE */
    /* U+0108 */ "c",   /* Ĉ  LATIN CAPITAL LETTER C WITH CIRCUMFLEX */
    /* U+0109 */ "c",   /* ĉ  LATIN SMALL LETTER C WITH CIRCUMFLEX */
    /* U+010A */ "c",   /* Ċ  LATIN CAPITAL LETTER C WITH DOT ABOVE */
    /* U+010B */ "c",   /* ċ  LATIN SMALL LETTER C WITH DOT ABOVE */
    /* U+010C */ "c",   /* Č  LATIN CAPITAL LETTER C WITH CARON */
    /* U+010D */ "c",   /* č  LATIN SMALL LETTER C WITH CARON */
    /* U+010E */ "d",   /* Ď  LATIN CAPITAL LETTER D WITH CARON */
    /* U+010F */ "d",   /* ď  LATIN SMALL LETTER D WITH CARON */
    /* U+0110 */ "d",   /* Đ  LATIN CAPITAL LETTER D WITH STROKE */
    /* U+0111 */ "d",   /* đ  LATIN SMALL LETTER D WITH STROKE */
    /* U+0112 */ "e",   /* Ē  LATIN CAPITAL LETTER E WITH MACRON */
    /* U+0113 */ "e",   /* ē  LATIN SMALL LETTER E WITH MACRON */
    /* U+0114 */ "e",   /* Ĕ  LATIN CAPITAL LETTER E WITH BREVE */
    /* U+0115 */ "e",   /* ĕ  LATIN SMALL LETTER E WITH BREVE */
    /* U+0116 */ "e",   /* Ė  LATIN CAPITAL LETTER E WITH DOT ABOVE */
    /* U+0117 */ "e",   /* ė  LATIN SMALL LETTER E WITH DOT ABOVE */
    /* U+0118 */ "e",   /* Ę  LATIN CAPITAL LETTER E WITH OGONEK */
    /* U+0119 */ "e",   /* ę  LATIN SMALL LETTER E WITH OGONEK */
    /* U+011A */ "e",   /* Ě  LATIN CAPITAL LETTER E WITH CARON */
    /* U+011B */ "e",   /* ě  LATIN SMALL LETTER E WITH CARON */
    /* U+011C */ "g",   /* Ĝ  LATIN CAPITAL LETTER G WITH CIRCUMFLEX */
    /* U+011D */ "g",   /* ĝ  LATIN SMALL LETTER G WITH CIRCUMFLEX */
    /* U+011E */ "g",   /* Ğ  LATIN CAPITAL LETTER G WITH BREVE */
    /* U+011F */ "g",   /* ğ  LATIN SMALL LETTER G WITH BREVE */
    /* U+0120 */ "g",   /* Ġ  LATIN CAPITAL LETTER G WITH DOT ABOVE */
    /* U+0121 */ "g",   /* ġ  LATIN SMALL LETTER G WITH DOT ABOVE */
    /* U+0122 */ "g",   /* Ģ  LATIN CAPITAL LETTER G WITH CEDILLA */
    /* U+0123 */ "g",   /* ģ  LATIN SMALL LETTER G WITH CEDILLA */
    /* U+0124 */ "h",   /* Ĥ  LATIN CAPITAL LETTER H WITH CIRCUMFLEX */
    /* U+0125 */ "h",   /* ĥ  LATIN SMALL LETTER H WITH CIRCUMFLEX */
    /* U+0126 */ "h",   /* Ħ  LATIN CAPITAL LETTER H WITH STROKE */
    /* U+0127 */ "h",   /* ħ  LATIN SMALL LETTER H WITH STROKE */
    /* U+0128 */ "i",   /* Ĩ  LATIN CAPITAL LETTER I WITH TILDE */
    /* U+0129 */ "i",   /* ĩ  LATIN SMALL LETTER I WITH TILDE */
    /* U+012A */ "i",   /* Ī  LATIN CAPITAL LETTER I WITH MACRON */
    /* U+012B */ "i",   /* ī  LATIN SMALL LETTER I WITH MACRON */
    /* U+012C */ "i",   /* Ĭ  LATIN CAPITAL LETTER I WITH BREVE */
    /* U+012D */ "i",   /* ĭ  LATIN SMALL LETTER I WITH BREVE */
    /* U+012E */ "i",   /* Į  LATIN CAPITAL LETTER I WITH OGONEK */
    /* U+012F */ "i",   /* į  LATIN SMALL LETTER I WITH OGONEK */
    /* U+0130 */ "i",   /* İ  LATIN CAPITAL LETTER I WITH DOT ABOVE */
    /* U+0131 */ "i",   /* ı  LATIN SMALL LETTER DOTLESS I */
    /* U+0132 */ "ij",  /* Ĳ  LATIN CAPITAL LIGATURE IJ */
    /* U+0133 */ "ij",  /* ĳ  LATIN SMALL LIGATURE IJ */
    /* U+0134 */ "j",   /* Ĵ  LATIN CAPITAL LETTER J WITH CIRCUMFLEX */
    /* U+0135 */ "j",   /* ĵ  LATIN SMALL LETTER J WITH CIRCUMFLEX */
    /* U+0136 */ "k",   /* Ķ  LATIN CAPITAL LETTER K WITH CEDILLA */
    /* U+0137 */ "k",   /* ķ  LATIN SMALL LETTER K WITH CEDILLA */
    /* U+0138 */ "k",   /* ĸ  LATIN SMALL LETTER KRA */
    /* U+0139 */ "l",   /* Ĺ  LATIN CAPITAL LETTER L WITH ACUTE */
    /* U+013A */ "l",   /* ĺ  LATIN SMALL LETTER L WITH ACUTE */
    /* U+013B */ "l",   /* Ļ  LATIN CAPITAL LETTER L WITH CEDILLA */
    /* U+013C */ "l",   /* ļ  LATIN SMALL LETTER L WITH CEDILLA */
    /* U+013D */ "l",   /* Ľ  LATIN CAPITAL LETTER L WITH CARON */
    /* U+013E */ "l",   /* ľ  LATIN SMALL LETTER L WITH CARON */
    /* U+013F */ "l",   /* Ŀ  LATIN CAPITAL LETTER L WITH MIDDLE DOT */
    /* U+0140 */ "l",   /* ŀ  LATIN SMALL LETTER L WITH MIDDLE DOT */
    /* U+0141 */ "l",   /* Ł  LATIN CAPITAL LETTER L WITH STROKE */
    /* U+0142 */ "l",   /* ł  LATIN SMALL LETTER L WITH STROKE */
    /* U+0143 */ "n",   /* Ń  LATIN CAPITAL LETTER N WITH ACUTE */
    /* U+0144 */ "n",   /* ń  LATIN SMALL LETTER N WITH ACUTE */
    /* U+0145 */ "n",   /* Ņ  LATIN CAPITAL LETTER N WITH CEDILLA */
    /* U+0146 */ "n",   /* ņ  LATIN SMALL LETTER N WITH CEDILLA */
    /* U+0147 */ "n",   /* Ň  LATIN CAPITAL LETTER N WITH CARON */
    /* U+0148 */ "n",   /* ň  LATIN SMALL LETTER N WITH CARON */
    /* U+0149 */ "n",   /* ŉ  LATIN SMALL LETTER N PRECEDED BY APOSTROPHE */
    /* U+014A */ "n",   /* Ŋ  LATIN CAPITAL LETTER ENG */
    /* U+014B */ "n",   /* ŋ  LATIN SMALL LETTER ENG */
    /* U+014C */ "o",   /* Ō  LATIN CAPITAL LETTER O WITH MACRON */
    /* U+014D */ "o",   /* ō  LATIN SMALL LETTER O WITH MACRON */
    /* U+014E */ "o",   /* Ŏ  LATIN CAPITAL LETTER O WITH BREVE */
    /* U+014F */ "o",   /* ŏ  LATIN SMALL LETTER O WITH BREVE */
    /* U+0150 */ "o",   /* Ő  LATIN CAPITAL LETTER O WITH DOUBLE ACUTE */
    /* U+0151 */ "o",   /* ő  LATIN SMALL LETTER O WITH DOUBLE ACUTE */
    /* U+0152 */ "oe",  /* Œ  LATIN CAPITAL LIGATURE OE */
    /* U+0153 */ "oe",  /* œ  LATIN SMALL LIGATURE OE */
    /* U+0154 */ "r",   /* Ŕ  LATIN CAPITAL LETTER R WITH ACUTE */
    /* U+0155 */ "r",   /* ŕ  LATIN SMALL LETTER R WITH ACUTE */
    /* U+0156 */ "r",   /* Ŗ  LATIN CAPITAL LETTER R WITH CEDILLA */
    /* U+0157 */ "r",   /* ŗ  LATIN SMALL LETTER R WITH CEDILLA */
    /* U+0158 */ "r",   /* Ř  LATIN CAPITAL LETTER R WITH CARON */
    /* U+0159 */ "r",   /* ř  LATIN SMALL LETTER R WITH CARON */
    /* U+015A */ "s",   /* Ś  LATIN CAPITAL LETTER S WITH ACUTE */
    /* U+015B */ "s",   /* ś  LATIN SMALL LETTER S WITH ACUTE */
    /* U+015C */ "s",   /* Ŝ  LATIN CAPITAL LETTER S WITH CIRCUMFLEX */
    /* U+015D */ "s",   /* ŝ  LATIN SMALL LETTER S WITH CIRCUMFLEX */
    /* U+015E */ "s",   /* Ş  LATIN CAPITAL LETTER S WITH CEDILLA */
    /* U+015F */ "s",   /* ş  LATIN SMALL LETTER S WITH CEDILLA */
    /* U+0160 */ "s",   /* Š  LATIN CAPITAL LETTER S WITH CARON */
    /* U+0161 */ "s",   /* š  LATIN SMALL LETTER S WITH CARON */
    /* U+0162 */ "t",   /* Ţ  LATIN CAPITAL LETTER T WITH CEDILLA */
    /* U+0163 */ "t",   /* ţ  LATIN SMALL LETTER T WITH CEDILLA */
    /* U+0164 */ "t",   /* Ť  LATIN CAPITAL LETTER T WITH CARON */
    /* U+0165 */ "t",   /* ť  LATIN SMALL LETTER T WITH CARON */
    /* U+0166 */ "t",   /* Ŧ  LATIN CAPITAL LETTER T WITH STROKE */
    /* U+0167 */ "t",   /* ŧ  LATIN SMALL LETTER T WITH STROKE */
    /* U+0168 */ "u",   /* Ũ  LATIN CAPITAL LETTER U WITH TILDE */
    /* U+0169 */ "u",   /* ũ  LATIN SMALL LETTER U WITH TILDE */
    /* U+016A */ "u",   /* Ū  LATIN CAPITAL LETTER U WITH MACRON */
    /* U+016B */ "u",   /* ū  LATIN SMALL LETTER U WITH MACRON */
    /* U+016C */ "u",   /* Ŭ  LATIN CAPITAL LETTER U WITH BREVE */
    /* U+016D */ "u",   /* ŭ  LATIN SMALL LETTER U WITH BREVE */
    /* U+016E */ "u",   /* Ů  LATIN CAPITAL LETTER U WITH RING ABOVE */
    /* U+016F */ "u",   /* ů  LATIN SMALL LETTER U WITH RING ABOVE */
    /* U+0170 */ "u",   /* Ű  LATIN CAPITAL LETTER U WITH DOUBLE ACUTE */
    /* U+0171 */ "u",   /* ű  LATIN SMALL LETTER U WITH DOUBLE ACUTE */
    /* U+0172 */ "u",   /* Ų  LATIN CAPITAL LETTER U WITH OGONEK */
    /* U+0173 */ "u",   /* ų  LATIN SMALL LETTER U WITH OGONEK */
    /* U+0174 */ "w",   /* Ŵ  LATIN CAPITAL LETTER W WITH CIRCUMFLEX */
    /* U+0175 */ "w",   /* ŵ  LATIN SMALL LETTER W WITH CIRCUMFLEX */
    /* U+0176 */ "y",   /* Ŷ  LATIN CAPITAL LETTER Y WITH CIRCUMFLEX */
    /* U+0177 */ "y",   /* ŷ  LATIN SMALL LETTER Y WITH CIRCUMFLEX */
    /* U+0178 */ "y",   /* Ÿ  LATIN CAPITAL LETTER Y WITH DIAERESIS */
    /* U+0179 */ "z",   /* Ź  LATIN CAPITAL LETTER Z WITH ACUTE */
    /* U+017A */ "z",   /* ź  LATIN SMALL LETTER Z WITH ACUTE */
    /* U+017B */ "z",   /* Ż  LATIN CAPITAL LETTER Z WITH DOT ABOVE */
    /* U+017C */ "z",   /* ż  LATIN SMALL LETTER Z WITH DOT ABOVE */
    /* U+017D */ "z",   /* Ž  LATIN CAPITAL LETTER Z WITH CARON */
    /* U+017E */ "z",   /* ž  LATIN SMALL LETTER Z WITH CARON */
    /* U+017F */ "s",   /* ſ  LATIN SMALL LETTER LONG S */
};

/*
 * Look up a codepoint in the transliteration table.
 * Returns a pointer to a 1-2 char ASCII string on match, NULL on no mapping.
 * Only covers U+00C0..U+017F.
 */
static const char *ipman_translit_lookup(uint32_t cp) {
    if (cp < 0x00C0 || cp > 0x017F) return NULL;
    return ipman_latin_translit[cp - 0x00C0];
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

    const unsigned char *s = (const unsigned char *)title;
    size_t remaining = strlen(title);

    while (remaining > 0) {
        uint32_t cp;
        size_t bytes_consumed;

        if (ipman_utf8_decode_next(s, remaining, &cp, &bytes_consumed) != 0) {
            /* Invalid UTF-8: skip exactly one byte and emit a dash */
            if (!last_was_dash && out_pos < max_len - 1) {
                out_slug[out_pos++] = '-';
                last_was_dash = 1;
            }
            s++;
            remaining--;
            continue;
        }

        if (cp < 0x80) {
            /* ASCII fast path: byte-identical behavior to original */
            char ch = (char)cp;
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
        } else {
            /* Non-ASCII: try transliteration */
            const char *mapped = ipman_translit_lookup(cp);
            if (mapped != NULL) {
                /* Append each lowercase char of the mapping */
                for (size_t k = 0; mapped[k] != '\0'; ++k) {
                    char mc = ipman_tolower(mapped[k]);
                    if (ipman_is_alnum(mc) && out_pos < max_len - 1) {
                        out_slug[out_pos++] = mc;
                        last_was_dash = 0;
                    }
                }
            } else {
                /* No mapping: emit dash (collapsed) */
                if (!last_was_dash && out_pos < max_len - 1) {
                    out_slug[out_pos++] = '-';
                    last_was_dash = 1;
                }
            }
        }

        s += bytes_consumed;
        remaining -= bytes_consumed;
    }

    if (out_pos > 0 && out_slug[out_pos - 1] == '-') {
        out_pos--;
    }
    out_slug[out_pos] = '\0';
}
