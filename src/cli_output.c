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

/* Split `text` into lines of at most `width` bytes each. Honors existing
 * `\n` as hard line breaks. Within a paragraph, breaks at the rightmost
 * space inside the width window when possible; otherwise hard-breaks at
 * the byte boundary.
 *
 * When width <= 0, or when text contains any byte >= 0x80 (start of a
 * multi-byte UTF-8 sequence), no wrapping is performed — the text is
 * returned as a single line. This keeps codepoints intact at the cost
 * of one unwrapped cell; pure-ASCII cells (the common case for the
 * project block) wrap as expected.
 *
 * On success, *lines_out is malloc'd and contains *count_out strdup'd
 * line buffers; the caller frees each lines[i] and lines itself.
 * Returns 0 on success, -1 on allocation failure (no partial output). */
static int wrap_cell(const char *text, int width,
                     char ***lines_out, int *count_out) {
    *lines_out = NULL;
    *count_out = 0;
    if (text == NULL) text = "";

    int can_wrap = (width > 0);
    if (can_wrap) {
        for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
            if (*p >= 0x80) { can_wrap = 0; break; }
        }
    }

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
        memcpy(arr[n], (buf), (blen)); \
        arr[n][(blen)] = '\0'; \
        n++; \
    } while (0)

    if (!can_wrap) {
        APPEND_LINE(text, strlen(text));
        *lines_out = arr;
        *count_out = (int)n;
        return 0;
    }

    const char *p = text;
    while (1) {
        const char *nl       = strchr(p, '\n');
        size_t      para_len = nl ? (size_t)(nl - p) : strlen(p);
        if (para_len == 0) {
            APPEND_LINE("", (size_t)0);
        } else {
            size_t start = 0;
            while (start < para_len) {
                size_t remaining = para_len - start;
                size_t take = remaining < (size_t)width ? remaining : (size_t)width;
                if (take < remaining) {
                    size_t break_at = 0;
                    for (size_t i = take; i > 0; i--) {
                        if (p[start + i - 1] == ' ') { break_at = i - 1; break; }
                    }
                    if (break_at == 0) break_at = take; /* no space → hard break */
                    APPEND_LINE(p + start, break_at);
                    start += break_at;
                    while (start < para_len && p[start] == ' ') start++;
                } else {
                    APPEND_LINE(p + start, take);
                    start += take;
                }
            }
        }
        if (nl == NULL) break;
        p = nl + 1;
    }

    if (n == 0) APPEND_LINE("", (size_t)0);

    *lines_out = arr;
    *count_out = (int)n;
    return 0;

oom:
    for (size_t i = 0; i < n; i++) free(arr[i]);
    free(arr);
    return -1;
#undef APPEND_LINE
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
            int len = (int)strlen(piece);
            if (len > w) len = w; /* defensive: don't overflow column */
            fputc(' ', out);
            fwrite(piece, 1, (size_t)len, out);
            for (int i = len; i < w; i++) fputc(' ', out);
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
