/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 *
 * Unit tests for ipman_slugify() — UTF-8 diacritic transliteration and
 * ASCII regression.
 *
 * Cases:
 *   - Spanish diacritics (consolidación, ñoño)
 *   - German diacritics (Müller, weiß)
 *   - French diacritics (café)
 *   - Polish diacritics (łódź)
 *   - Invalid UTF-8 bytes (must not crash, replace with -)
 *   - ASCII regression: byte-identical output for ASCII-only inputs
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "validation.h"

static int g_failures = 0;
static const char *g_case = "(none)";

#define FAIL(msg) do { \
    fprintf(stderr, "  FAIL [%s] %s (at %s:%d)\n", \
            g_case, (msg), __FILE__, __LINE__); \
    g_failures++; \
    return; \
} while (0)

#define ASSERT_STR_EQ(actual, expected) do { \
    if (strcmp((actual), (expected)) != 0) { \
        fprintf(stderr, "  FAIL [%s] expected slug '%s', got '%s' (at %s:%d)\n", \
                g_case, (expected), (actual), __FILE__, __LINE__); \
        g_failures++; \
        return; \
    } \
} while (0)

static void check_slug(const char *input, const char *expected) {
    char buf[256];
    ipman_slugify(input, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, expected);
}

/* --- Spanish ---------------------------------------------------------------- */

static void case_spanish_consolidacion(void) {
    g_case = "spanish: consolidación -> consolidacion";
    /* consolidación: c-o-n-s-o-l-i-d-a-c-i-ó-n  (ó = U+00F3) */
    check_slug("consolidaci\xC3\xB3n", "consolidacion");
}

static void case_spanish_nono(void) {
    g_case = "spanish: ñoño -> nono";
    /* ñ = U+00F1 (0xC3 0xB1) */
    check_slug("\xC3\xB1o\xC3\xB1o", "nono");
}

/* --- German ----------------------------------------------------------------- */

static void case_german_muller(void) {
    g_case = "german: Müller -> muller";
    /* ü = U+00FC (0xC3 0xBC) */
    check_slug("M\xC3\xBCller", "muller");
}

static void case_german_weiss(void) {
    g_case = "german: weiß -> weiss";
    /* ß = U+00DF (0xC3 0x9F) */
    check_slug("wei\xC3\x9F", "weiss");
}

/* --- French ----------------------------------------------------------------- */

static void case_french_cafe(void) {
    g_case = "french: café -> cafe";
    /* é = U+00E9 (0xC3 0xA9) */
    check_slug("caf\xC3\xA9", "cafe");
}

/* --- Polish ----------------------------------------------------------------- */

static void case_polish_lodz(void) {
    g_case = "polish: łódź -> lodz";
    /* ł = U+0142 (0xC5 0x82), ó = U+00F3 (0xC3 0xB3), ź = U+017A (0xC5 0xBA) */
    /* Split the literal to prevent \xBAd being parsed as a single escape */
    check_slug("\xC5\x82\xC3\xB3" "d\xC5\xBA", "lodz");
}

/* --- Invalid UTF-8 ---------------------------------------------------------- */

static void case_invalid_utf8_no_crash(void) {
    g_case = "invalid UTF-8: must not crash";
    /* Lone continuation byte, overlong sequence, truncated sequence */
    char buf[256];
    /* \x80 is a lone continuation byte */
    ipman_slugify("a\x80z", buf, sizeof(buf));
    /* \x80 is a lone continuation byte -> '-', collapsed: "a-z" */
    ASSERT_STR_EQ(buf, "a-z");
}

static void case_invalid_utf8_dash_substitution(void) {
    g_case = "invalid UTF-8: invalid bytes become dash separators";
    char buf[256];
    /* \xFF is never valid in UTF-8 */
    ipman_slugify("a\xFFz", buf, sizeof(buf));
    /* 'a', invalid byte (->-), 'z' => "a-z" */
    ASSERT_STR_EQ(buf, "a-z");
}

/* --- ASCII regression (Task #88) ------------------------------------------- */

static void case_ascii_hello_world(void) {
    g_case = "ascii: Hello World -> hello-world";
    check_slug("Hello World", "hello-world");
}

static void case_ascii_foo_bar_baz(void) {
    g_case = "ascii: foo-bar_baz -> foo-bar-baz";
    check_slug("foo-bar_baz", "foo-bar-baz");
}

static void case_ascii_leading_trailing_spaces(void) {
    g_case = "ascii: leading and trailing spaces trimmed";
    check_slug("  leading and trailing  ", "leading-and-trailing");
}

static void case_ascii_numbers(void) {
    g_case = "ascii: numbers preserved";
    check_slug("Phase 2 Go", "phase-2-go");
}

static void case_ascii_all_separators(void) {
    g_case = "ascii: all separators -> empty";
    check_slug("   ---   ", "");
}

static void case_ascii_empty(void) {
    g_case = "ascii: empty string -> empty";
    check_slug("", "");
}

static void case_ascii_null(void) {
    g_case = "ascii: NULL input -> empty";
    char buf[64] = "guard";
    ipman_slugify(NULL, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "");
}

static void case_ascii_max_len_zero(void) {
    g_case = "ascii: max_len==0 is no-op";
    char buf[4] = {0x42, 0x42, 0x42, 0x42};
    ipman_slugify("hello", buf, 0);
    /* buf must not be touched at all */
    if (buf[0] != 0x42) {
        fprintf(stderr, "  FAIL [%s] buffer was modified when max_len==0 (at %s:%d)\n",
                g_case, __FILE__, __LINE__);
        g_failures++;
    }
}

int main(void) {
    /* Spanish */
    case_spanish_consolidacion();
    case_spanish_nono();

    /* German */
    case_german_muller();
    case_german_weiss();

    /* French */
    case_french_cafe();

    /* Polish */
    case_polish_lodz();

    /* Invalid UTF-8 */
    case_invalid_utf8_no_crash();
    case_invalid_utf8_dash_substitution();

    /* ASCII regression (Task #88) */
    case_ascii_hello_world();
    case_ascii_foo_bar_baz();
    case_ascii_leading_trailing_spaces();
    case_ascii_numbers();
    case_ascii_all_separators();
    case_ascii_empty();
    case_ascii_null();
    case_ascii_max_len_zero();

    if (g_failures > 0) {
        fprintf(stderr, "test_slugify: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_slugify: all 16 cases passed\n");
    return 0;
}
