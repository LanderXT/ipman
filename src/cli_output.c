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
        int w   = t->col_width[c];
        int len = (int)strlen(t->headers[c]);
        int lpad = (w - len) / 2;
        int rpad = w - len - lpad;
        fputc(' ', out);
        for (int i = 0; i < lpad; i++) fputc(' ', out);
        fputs(t->headers[c], out);
        for (int i = 0; i < rpad; i++) fputc(' ', out);
        fputc(' ', out);
        fputs(k_v, out);
    }
    fputc('\n', out);
}

static void print_data_row(FILE *out, const cli_table_t *t, int row) {
    fputs(k_v, out);
    for (int c = 0; c < t->col_count; c++) {
        int         w    = t->col_width[c];
        const char *cell = t->cells[row][c] ? t->cells[row][c] : "";
        int         len  = (int)strlen(cell);
        fputc(' ', out);
        fputs(cell, out);
        for (int i = len; i < w; i++) fputc(' ', out);
        fputc(' ', out);
        fputs(k_v, out);
    }
    fputc('\n', out);
}

void cli_table_init(cli_table_t *t, int col_count, const char * const *headers) {
    memset(t, 0, sizeof *t);
    t->col_count = col_count < CLI_TABLE_MAX_COLS ? col_count : CLI_TABLE_MAX_COLS;
    for (int c = 0; c < t->col_count; c++) {
        t->headers[c]   = strdup(headers[c] ? headers[c] : "");
        t->col_width[c] = (int)strlen(t->headers[c]);
    }
}

int cli_table_add_row(cli_table_t *t, const char * const *cells) {
    if (t->row_count >= CLI_TABLE_MAX_ROWS) return -1;
    int r = t->row_count++;
    for (int c = 0; c < t->col_count; c++) {
        const char *s   = (cells && cells[c]) ? cells[c] : "";
        t->cells[r][c]  = strdup(s);
        int len = (int)strlen(s);
        if (len > t->col_width[c]) t->col_width[c] = len;
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
