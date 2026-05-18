/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Homero Leal
 */

#ifndef IPMAN_CLI_OUTPUT_H
#define IPMAN_CLI_OUTPUT_H

#include <stdio.h>

#define CLI_TABLE_MAX_COLS 8
#define CLI_TABLE_MAX_ROWS 512

/*
 * Simple table renderer that produces Unicode box-drawing output.
 * Column widths are derived automatically from headers and cell content.
 * Headers are centered; data rows are left-aligned.
 *
 * Optional per-column max width: when set, cells exceeding the cap wrap
 * onto multiple visual lines within the same logical row. Embedded `\n`
 * in cell content also forces a line break. Wrapping only kicks in for
 * pure-ASCII cells; cells containing multi-byte characters render
 * unwrapped to avoid splitting a UTF-8 codepoint mid-byte.
 *
 * Usage:
 *   cli_table_t t;
 *   const char *headers[] = {"Ref", "Status", "Title"};
 *   cli_table_init(&t, 3, headers);
 *   cli_table_set_col_max_width(&t, 2, 60);  // optional: wrap Title at 60
 *   const char *row[] = {"P1/T1", "todo", "Fix login bug"};
 *   cli_table_add_row(&t, row);
 *   cli_table_print(&t, stdout);
 *   cli_table_free(&t);
 */
typedef struct {
    int   col_count;
    char *headers[CLI_TABLE_MAX_COLS];
    int   col_width[CLI_TABLE_MAX_COLS];
    int   col_max_width[CLI_TABLE_MAX_COLS]; /* 0 = unlimited */
    int   row_count;
    char *cells[CLI_TABLE_MAX_ROWS][CLI_TABLE_MAX_COLS];
} cli_table_t;

void cli_table_init(cli_table_t *t, int col_count, const char * const *headers);

/* Set a per-column max width. Pass max_width <= 0 to disable wrapping
 * for that column (the default). Must be called before cli_table_add_row
 * so the cap is honored when col_width is computed. */
void cli_table_set_col_max_width(cli_table_t *t, int col, int max_width);

/* Returns 0 on success, -1 if the row limit is reached. NULL cells render as "". */
int  cli_table_add_row(cli_table_t *t, const char * const *cells);

void cli_table_print(const cli_table_t *t, FILE *out);
void cli_table_free(cli_table_t *t);

#endif /* IPMAN_CLI_OUTPUT_H */
