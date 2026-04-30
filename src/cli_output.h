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
 * Usage:
 *   cli_table_t t;
 *   const char *headers[] = {"Ref", "Status", "Title"};
 *   cli_table_init(&t, 3, headers);
 *   const char *row[] = {"P1/T1", "todo", "Fix login bug"};
 *   cli_table_add_row(&t, row);
 *   cli_table_print(&t, stdout);
 *   cli_table_free(&t);
 */
typedef struct {
    int   col_count;
    char *headers[CLI_TABLE_MAX_COLS];
    int   col_width[CLI_TABLE_MAX_COLS];
    int   row_count;
    char *cells[CLI_TABLE_MAX_ROWS][CLI_TABLE_MAX_COLS];
} cli_table_t;

void cli_table_init(cli_table_t *t, int col_count, const char * const *headers);

/* Returns 0 on success, -1 if the row limit is reached. NULL cells render as "". */
int  cli_table_add_row(cli_table_t *t, const char * const *cells);

void cli_table_print(const cli_table_t *t, FILE *out);
void cli_table_free(cli_table_t *t);

#endif /* IPMAN_CLI_OUTPUT_H */
