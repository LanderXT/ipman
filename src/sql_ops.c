#include "sql_ops.h"

#include "log.h"

#include <ctype.h>

static int is_statement_separator(char c) {
    return c == ';' || isspace((unsigned char)c);
}

int ipman_sql_run(sqlite3 *db, const char *sql, FILE *out) {
    const char *cursor = sql;
    while (*cursor) {
        while (*cursor && is_statement_separator(*cursor)) cursor++;
        if (!*cursor) break;

        sqlite3_stmt *stmt = NULL;
        const char *next = NULL;
        int rc = sqlite3_prepare_v2(db, cursor, -1, &stmt, &next);
        if (rc != SQLITE_OK) {
            ipman_log_error("sql prepare failed",
                           "rc=%d msg=%s", rc, sqlite3_errmsg(db));
            if (stmt != NULL) sqlite3_finalize(stmt);
            return -1;
        }
        if (stmt == NULL) {
            /* Trailing whitespace-only statement -- prepare succeeds with
             * a NULL stmt; nothing to step. */
            cursor = next;
            continue;
        }

        int ncol = sqlite3_column_count(stmt);
        for (;;) {
            rc = sqlite3_step(stmt);
            if (rc == SQLITE_ROW) {
                for (int i = 0; i < ncol; i++) {
                    const unsigned char *v = sqlite3_column_text(stmt, i);
                    fputs(v ? (const char *)v : "", out);
                    if (i + 1 < ncol) fputc('|', out);
                }
                fputc('\n', out);
                continue;
            }
            break;
        }
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            ipman_log_error("sql step failed",
                           "rc=%d msg=%s", rc, sqlite3_errmsg(db));
            return -1;
        }
        cursor = next;
    }
    return 0;
}
