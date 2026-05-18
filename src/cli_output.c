/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#include "cli_output.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* UTF-8 box-drawing characters (3 bytes each, 1 display column each). */
static const char k_h[]  = "─";
static const char k_tl[] = "┌";
static const char k_tr[] = "┐";
static const char k_bl[] = "└";
static const char k_br[] = "┘";
static const char k_lm[] = "├";
static const char k_rm[] = "┤";
static const char k_tm[] = "┬";
static const char k_bm[] = "┴";
static const char k_x[]  = "┼";
static const char k_v[]  = "│";

static void print_horiz(FILE *out, int n) {
    for (int i = 0; i < n; i++) fputs(k_h, out);
}

static void print_border(FILE *out, const cli_table_t *t,
                         const char *left, const char *mid, const char *right) {
    fputs(left, out);
    for (int c = 0; c < t->col_count; c++) {
        print_horiz(out, t->col_width[c] + 2);
        fputs(c + 1 < t->col_count ? mid : right, out);
    }
    fputc('\n', out);
}

static void print_header_row(FILE *out, const cli_table_t *t) {
    fputs(k_v, out);
    for (int c = 0; c < t->col_count; c++) {
        int         w    = t->col_width[c];
        const char *h    = t->headers[c] ? t->headers[c] : "";
        int         len  = (int)strlen(h);
        int         lpad = (w - len) / 2;
        int         rpad = w - len - lpad;
        fputc(' ', out);
        for (int i = 0; i < lpad; i++) fputc(' ', out);
        fputs(h, out);
        for (int i = 0; i < rpad; i++) fputc(' ', out);
        fputc(' ', out);
        fputs(k_v, out);
    }
    fputc('\n', out);
}

/* Byte length of the UTF-8 sequence whose lead byte is `c`.
 * Invalid lead bytes (continuation bytes, 0xF8+) return 1 so the walker
 * keeps moving forward instead of looping. */
static int utf8_seq_len(unsigned char c) {
    if (c < 0x80) return 1;
    if (c < 0xC0) return 1; /* stray continuation byte: treat as 1 */
    if (c < 0xE0) return 2;
    if (c < 0xF0) return 3;
    if (c < 0xF8) return 4;
    return 1;
}

/* Display-column count of NUL-terminated UTF-8 `s`. Approximates one
 * column per codepoint; does not handle wide (CJK) or zero-width
 * characters. Matches the strlen-based col_width math used elsewhere
 * for ASCII while keeping multi-byte cells visually aligned. */
static size_t utf8_display_cols(const char *s) {
    size_t cols = 0;
    while (*s) {
        s += utf8_seq_len((unsigned char)*s);
        cols++;
    }
    return cols;
}

/* Split `text` into lines of at most `width` display columns each.
 * Honors existing `\n` as hard line breaks. Within a paragraph, breaks
 * at the rightmost space inside the width window when possible; if no
 * space is available, hard-breaks at the next codepoint boundary so
 * UTF-8 sequences are never split mid-byte.
 *
 * width <= 0 disables wrapping (a single line per `\n`-delimited
 * paragraph is emitted regardless of length). On success, *lines_out
 * is malloc'd and *count_out lines are each malloc'd strings; the
 * caller frees each lines[i] and lines itself. Returns -1 on alloc
 * failure with no partial output. */
static int wrap_cell(const char *text, int width,
                     char ***lines_out, int *count_out) {
    *lines_out = NULL;
    *count_out = 0;
    if (text == NULL) text = "";
    size_t text_len = strlen(text);

    size_t cap = 8, n = 0;
    char **arr = malloc(cap * sizeof *arr);
    if (arr == NULL) return -1;

#define APPEND_LINE(buf, blen) do { \
        if (n == cap) { \
            size_t new_cap = cap * 2; \
            char **na = realloc(arr, new_cap * sizeof *arr); \
            if (na == NULL) goto oom; \
            arr = na; cap = new_cap; \
        } \
        arr[n] = malloc((blen) + 1); \
        if (arr[n] == NULL) goto oom; \
        if ((blen) > 0) memcpy(arr[n], (buf), (blen)); \
        arr[n][(blen)] = '\0'; \
        n++; \
    } while (0)

    /* width <= 0 path: split only at `\n`, no width-based wrapping. */
    if (width <= 0) {
        size_t para_start = 0;
        for (size_t i = 0; i <= text_len; i++) {
            if (i == text_len || text[i] == '\n') {
                APPEND_LINE(text + para_start, i - para_start);
                para_start = i + 1;
                if (i == text_len) break;
            }
        }
        if (n == 0) APPEND_LINE("", (size_t)0);
        *lines_out = arr;
        *count_out = (int)n;
        return 0;
    }

    /* Codepoint walk for width-based wrapping. line_start is the byte
     * offset where the current visual line begins; line_cols is the
     * display-column count emitted on it so far; last_space is the byte
     * offset of the most recent ASCII space in the current line, or
     * SIZE_MAX if none. */
    size_t i          = 0;
    size_t line_start = 0;
    size_t line_cols  = 0;
    size_t last_space = (size_t)-1;

    while (i < text_len) {
        if (text[i] == '\n') {
            APPEND_LINE(text + line_start, i - line_start);
            line_start = i + 1;
            line_cols  = 0;
            last_space = (size_t)-1;
            i++;
            continue;
        }
        int seq = utf8_seq_len((unsigned char)text[i]);
        if (i + (size_t)seq > text_len) seq = (int)(text_len - i);

        if (line_cols + 1 > (size_t)width) {
            /* Wrap before consuming this codepoint. */
            if (last_space != (size_t)-1) {
                APPEND_LINE(text + line_start, last_space - line_start);
                line_start = last_space + 1;
                while (line_start < text_len && text[line_start] == ' ') line_start++;
            } else {
                APPEND_LINE(text + line_start, i - line_start);
                line_start = i;
            }
            /* Recompute line_cols and last_space for the residue between
             * line_start and i (the codepoints we skipped past when
             * unwinding to the chosen break point). */
            line_cols  = 0;
            last_space = (size_t)-1;
            for (size_t k = line_start; k < i; ) {
                int sl = utf8_seq_len((unsigned char)text[k]);
                if (k + (size_t)sl > i) sl = (int)(i - k);
                if (text[k] == ' ') last_space = k;
                line_cols++;
                k += (size_t)sl;
            }
        }

        if (text[i] == ' ') last_space = i;
        line_cols++;
        i += (size_t)seq;
    }

    /* Trailing partial line. A text ending in `\n` won't enter here
     * because line_start has already advanced past the last `\n`. */
    if (line_start < text_len) {
        APPEND_LINE(text + line_start, text_len - line_start);
    }
    if (n == 0) APPEND_LINE("", (size_t)0);

#undef APPEND_LINE

    *lines_out = arr;
    *count_out = (int)n;
    return 0;

oom:
    for (size_t k = 0; k < n; k++) free(arr[k]);
    free(arr);
    return -1;
}

static void print_data_row(FILE *out, const cli_table_t *t, int row) {
    char **wrapped[CLI_TABLE_MAX_COLS] = {0};
    int    line_count[CLI_TABLE_MAX_COLS] = {0};
    int    max_lines = 1;

    for (int c = 0; c < t->col_count; c++) {
        const char *cell = t->cells[row][c] ? t->cells[row][c] : "";
        if (wrap_cell(cell, t->col_width[c], &wrapped[c], &line_count[c]) != 0) {
            /* Allocation failure: render the cell as a single line of raw
             * content. Worst case mirrors the pre-wrapping behavior. */
            wrapped[c]    = NULL;
            line_count[c] = 1;
        }
        if (line_count[c] > max_lines) max_lines = line_count[c];
    }

    for (int line = 0; line < max_lines; line++) {
        fputs(k_v, out);
        for (int c = 0; c < t->col_count; c++) {
            int         w     = t->col_width[c];
            const char *piece = "";
            if (wrapped[c] != NULL) {
                piece = (line < line_count[c]) ? wrapped[c][line] : "";
            } else if (line == 0) {
                piece = t->cells[row][c] ? t->cells[row][c] : "";
            }
            size_t byte_len = strlen(piece);
            size_t disp     = utf8_display_cols(piece);
            fputc(' ', out);
            fwrite(piece, 1, byte_len, out);
            /* Pad to column width in display columns. If disp > w (no
             * wrap path with a long line), emit no padding so the cell
             * overflows visually rather than truncating content. */
            if (disp < (size_t)w) {
                for (size_t i = disp; i < (size_t)w; i++) fputc(' ', out);
            }
            fputc(' ', out);
            fputs(k_v, out);
        }
        fputc('\n', out);
    }

    for (int c = 0; c < t->col_count; c++) {
        if (wrapped[c] == NULL) continue;
        for (int i = 0; i < line_count[c]; i++) free(wrapped[c][i]);
        free(wrapped[c]);
    }
}

void cli_table_init(cli_table_t *t, int col_count, const char * const *headers) {
    memset(t, 0, sizeof *t);
    t->col_count = col_count < CLI_TABLE_MAX_COLS ? col_count : CLI_TABLE_MAX_COLS;
    for (int c = 0; c < t->col_count; c++) {
        const char *h  = headers[c] ? headers[c] : "";
        t->headers[c]  = strdup(h);
        t->col_width[c] = (int)strlen(h);
        /* col_max_width[c] is 0 (unlimited) by virtue of the memset above. */
    }
}

void cli_table_set_col_max_width(cli_table_t *t, int col, int max_width) {
    if (col < 0 || col >= t->col_count) return;
    t->col_max_width[col] = max_width > 0 ? max_width : 0;
}

/* Effective column width for a cell of `cell_len` bytes: capped by
 * col_max_width when set, but never below the header width (so the
 * header still renders cleanly when content is short). */
static int effective_col_width(const cli_table_t *t, int col, int cell_len) {
    int w = cell_len;
    int cap = t->col_max_width[col];
    if (cap > 0 && w > cap) w = cap;
    int header_w = (int)strlen(t->headers[col] ? t->headers[col] : "");
    if (w < header_w) w = header_w;
    return w;
}

int cli_table_add_row(cli_table_t *t, const char * const *cells) {
    if (t->row_count >= CLI_TABLE_MAX_ROWS) return -1;
    int r = t->row_count++;
    for (int c = 0; c < t->col_count; c++) {
        const char *s   = (cells && cells[c]) ? cells[c] : "";
        t->cells[r][c]  = strdup(s);
        int candidate = effective_col_width(t, c, (int)strlen(s));
        if (candidate > t->col_width[c]) t->col_width[c] = candidate;
    }
    return 0;
}

void cli_table_print(const cli_table_t *t, FILE *out) {
    if (t->col_count == 0) return;
    print_border(out, t, k_tl, k_tm, k_tr);
    print_header_row(out, t);
    if (t->row_count == 0) {
        print_border(out, t, k_bl, k_bm, k_br);
        return;
    }
    print_border(out, t, k_lm, k_x, k_rm);
    for (int r = 0; r < t->row_count; r++) {
        print_data_row(out, t, r);
        if (r + 1 < t->row_count)
            print_border(out, t, k_lm, k_x, k_rm);
        else
            print_border(out, t, k_bl, k_bm, k_br);
    }
}

void cli_table_free(cli_table_t *t) {
    for (int c = 0; c < t->col_count; c++) {
        free(t->headers[c]);
        t->headers[c] = NULL;
    }
    for (int r = 0; r < t->row_count; r++) {
        for (int c = 0; c < t->col_count; c++) {
            free(t->cells[r][c]);
            t->cells[r][c] = NULL;
        }
    }
    t->row_count = 0;
    t->col_count = 0;
}
