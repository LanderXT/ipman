#include "render_md.h"

#include <cJSON.h>
#include <sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* String / display helpers                                           */
/* ------------------------------------------------------------------ */

static const char *k_months[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
};

/* Parse the YYYY-MM-DD prefix of an ISO 8601 string into "Mon D, YYYY". */
static void format_date(const char *iso, char *buf, size_t len) {
    if (iso == NULL || iso[0] == '\0') { snprintf(buf, len, "—"); return; }
    int y = 0, m = 0, d = 0;
    if (sscanf(iso, "%d-%d-%d", &y, &m, &d) == 3 && m >= 1 && m <= 12) {
        snprintf(buf, len, "%s %d, %d", k_months[m - 1], d, y);
        return;
    }
    snprintf(buf, len, "%s", iso);
}

static void format_today(char *buf, size_t len) {
    time_t now = time(NULL);
    struct tm tm_local;
    localtime_r(&now, &tm_local);
    int m = tm_local.tm_mon + 1;
    snprintf(buf, len, "%s %d, %d",
             k_months[m - 1], tm_local.tm_mday, 1900 + tm_local.tm_year);
}

static const char *label_plan_status(const char *s) {
    if (s == NULL)                        return "—";
    if (strcmp(s, "open")        == 0)    return "Open";
    if (strcmp(s, "in_progress") == 0)    return "In Progress";
    if (strcmp(s, "paused")      == 0)    return "Paused";
    if (strcmp(s, "completed")   == 0)    return "Completed";
    if (strcmp(s, "canceled")    == 0)    return "Canceled";
    if (strcmp(s, "archived")    == 0)    return "Archived";
    return s;
}

static const char *label_phase_status(const char *s) {
    if (s == NULL)                        return "—";
    if (strcmp(s, "open")        == 0)    return "Open";
    if (strcmp(s, "in_progress") == 0)    return "In Progress";
    if (strcmp(s, "blocked")     == 0)    return "Blocked";
    if (strcmp(s, "completed")   == 0)    return "Completed";
    if (strcmp(s, "canceled")    == 0)    return "Canceled";
    return s;
}

static const char *label_task_status(const char *s) {
    if (s == NULL)                        return "—";
    if (strcmp(s, "todo")        == 0)    return "Todo";
    if (strcmp(s, "in_progress") == 0)    return "In Progress";
    if (strcmp(s, "blocked")     == 0)    return "Blocked";
    if (strcmp(s, "deferred")    == 0)    return "Deferred";
    if (strcmp(s, "done")        == 0)    return "Done";
    if (strcmp(s, "canceled")    == 0)    return "Canceled";
    return s;
}

static const char *label_priority(const char *s) {
    if (s == NULL)                  return "—";
    if (strcmp(s, "low")      == 0) return "Low";
    if (strcmp(s, "medium")   == 0) return "Medium";
    if (strcmp(s, "high")     == 0) return "High";
    if (strcmp(s, "critical") == 0) return "Critical";
    return s;
}

static const char *label_task_type(const char *s) {
    if (s == NULL)                        return "Task";
    if (strcmp(s, "task")          == 0)  return "Task";
    if (strcmp(s, "research")      == 0)  return "Research";
    if (strcmp(s, "bug")           == 0)  return "Bug";
    if (strcmp(s, "decision")      == 0)  return "Decision";
    if (strcmp(s, "review")        == 0)  return "Review";
    if (strcmp(s, "documentation") == 0)  return "Docs";
    return s;
}

/* Print a 20-character block progress bar: filled=█ empty=░ */
static void render_progress_bar(FILE *out, int done, int total) {
    int filled = (total > 0) ? (done * 20 / total) : 0;
    if (filled > 20) filled = 20;
    for (int i = 0; i < 20; i++) {
        fputs(i < filled ? "\xe2\x96\x88" : "\xe2\x96\x91", out);
    }
}

/* Print a tags JSON array as comma-separated backtick-quoted strings. */
static void render_tags(FILE *out, const char *tags_json) {
    if (tags_json == NULL || tags_json[0] == '\0') { fputs("—", out); return; }
    cJSON *arr = cJSON_Parse(tags_json);
    if (arr == NULL || !cJSON_IsArray(arr)) {
        if (arr != NULL) cJSON_Delete(arr);
        fputs(tags_json, out);
        return;
    }
    int first = 1;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        if (cJSON_IsString(item) && item->valuestring) {
            if (!first) fputs(", ", out);
            fprintf(out, "`%s`", item->valuestring);
            first = 0;
        }
    }
    if (first) fputs("—", out);
    cJSON_Delete(arr);
}

/* ------------------------------------------------------------------ */
/* Plan resolution                                                    */
/* ------------------------------------------------------------------ */

static sqlite3_int64 query_plan_id(sqlite3 *db, const char *sql,
                                   const char *value) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, value, -1, SQLITE_STATIC);
    sqlite3_int64 id = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) id = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return id;
}

int ipman_render_resolve_plan(sqlite3 *db, const char *selector,
                             sqlite3_int64 *plan_id_out) {
    sqlite3_int64 id;

    id = query_plan_id(db, "SELECT id FROM plans WHERE code = ?",  selector);
    if (id > 0) { *plan_id_out = id; return 0; }

    id = query_plan_id(db, "SELECT id FROM plans WHERE uid = ?",   selector);
    if (id > 0) { *plan_id_out = id; return 0; }

    id = query_plan_id(db, "SELECT id FROM plans WHERE label = ?", selector);
    if (id > 0) { *plan_id_out = id; return 0; }

    /* Try as a bare numeric id (e.g. "3"). */
    char *end = NULL;
    long long num = strtoll(selector, &end, 10);
    if (end != selector && *end == '\0' && num > 0) {
        id = query_plan_id(db, "SELECT id FROM plans WHERE id = ?", selector);
        if (id > 0) { *plan_id_out = id; return 0; }
    }

    return -1;
}

/* ------------------------------------------------------------------ */
/* Plan data loader                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    sqlite3_int64 id;
    char *code;
    char *title;
    char *summary;
    char *description;
    char *status;
    char *priority;
    char *created_at;
    char *updated_at;
    char *opened_at;
    char *closed_at;
    char *archived_at;
    char *owner;
    char *target_date;
    char *tags_json;
    char *version_label;
    char *uid;
    char *label;
} plan_data_t;

static char *dup_col(sqlite3_stmt *stmt, int col) {
    const unsigned char *v = sqlite3_column_text(stmt, col);
    return v ? strdup((const char *)v) : NULL;
}

static void plan_data_free(plan_data_t *p) {
    free(p->code);         free(p->title);        free(p->summary);
    free(p->description);  free(p->status);        free(p->priority);
    free(p->created_at);   free(p->updated_at);   free(p->opened_at);
    free(p->closed_at);    free(p->archived_at);  free(p->owner);
    free(p->target_date);  free(p->tags_json);    free(p->version_label);
    free(p->uid);          free(p->label);
}

static int load_plan_data(sqlite3 *db, sqlite3_int64 plan_id, plan_data_t *p) {
    const char *sql =
        "SELECT id, code, title, summary, description, status, priority, "
        "created_at, updated_at, opened_at, closed_at, archived_at, owner, "
        "target_date, tags, version_label, uid, label "
        "FROM plans WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, plan_id);
    if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); return -1; }

    memset(p, 0, sizeof *p);
    p->id            = sqlite3_column_int64(stmt, 0);
    p->code          = dup_col(stmt,  1);
    p->title         = dup_col(stmt,  2);
    p->summary       = dup_col(stmt,  3);
    p->description   = dup_col(stmt,  4);
    p->status        = dup_col(stmt,  5);
    p->priority      = dup_col(stmt,  6);
    p->created_at    = dup_col(stmt,  7);
    p->updated_at    = dup_col(stmt,  8);
    p->opened_at     = dup_col(stmt,  9);
    p->closed_at     = dup_col(stmt, 10);
    p->archived_at   = dup_col(stmt, 11);
    p->owner         = dup_col(stmt, 12);
    p->target_date   = dup_col(stmt, 13);
    p->tags_json     = dup_col(stmt, 14);
    p->version_label = dup_col(stmt, 15);
    p->uid           = dup_col(stmt, 16);
    p->label         = dup_col(stmt, 17);
    sqlite3_finalize(stmt);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Section renderers                                                  */
/* ------------------------------------------------------------------ */

static void render_header(FILE *out, const plan_data_t *p) {
    char today[64];
    format_today(today, sizeof today);

    /* YAML front matter — consumed by pandoc for PDF/HTML metadata. */
    fprintf(out, "---\n");
    fprintf(out, "title: \"%s\"\n", p->title ? p->title : "");
    fprintf(out, "subtitle: \"Implementation Plan\"\n");
    if (p->owner && p->owner[0] != '\0')
        fprintf(out, "author: \"%s\"\n", p->owner);
    fprintf(out, "date: \"%s\"\n", today);
    fprintf(out, "---\n\n");

    /* Document title. */
    fprintf(out, "# %s\n\n", p->title ? p->title : "Implementation Plan");

    /* One-line key identifiers. */
    fprintf(out, "**Code:** `%s`", p->code ? p->code : "—");
    fprintf(out, " · **Status:** %s", label_plan_status(p->status));
    if (p->priority && p->priority[0] != '\0')
        fprintf(out, " · **Priority:** %s", label_priority(p->priority));
    fprintf(out, "\n\n---\n\n");
}

static void render_overview(FILE *out, const plan_data_t *p) {
    char date_created[64], date_updated[64], date_target[64];
    format_date(p->created_at,  date_created, sizeof date_created);
    format_date(p->updated_at,  date_updated, sizeof date_updated);
    format_date(p->target_date, date_target,  sizeof date_target);

    fprintf(out, "## Plan Overview\n\n");
    fprintf(out, "| | |\n");
    fprintf(out, "|:--|:--|\n");
    fprintf(out, "| **Code** | `%s` |\n", p->code ? p->code : "—");
    fprintf(out, "| **Status** | %s |\n",  label_plan_status(p->status));
    if (p->priority && p->priority[0] != '\0')
        fprintf(out, "| **Priority** | %s |\n", label_priority(p->priority));
    if (p->owner && p->owner[0] != '\0')
        fprintf(out, "| **Owner** | %s |\n", p->owner);
    if (p->target_date && p->target_date[0] != '\0')
        fprintf(out, "| **Target Date** | %s |\n", date_target);
    if (p->version_label && p->version_label[0] != '\0')
        fprintf(out, "| **Version** | %s |\n", p->version_label);
    fprintf(out, "| **Created** | %s |\n",      date_created);
    fprintf(out, "| **Last Updated** | %s |\n", date_updated);
    if (p->tags_json && p->tags_json[0] != '\0') {
        fprintf(out, "| **Tags** | ");
        render_tags(out, p->tags_json);
        fprintf(out, " |\n");
    }
    fprintf(out, "\n");

    if (p->summary && p->summary[0] != '\0')
        fprintf(out, "> %s\n\n", p->summary);

    fprintf(out, "---\n\n");
}

static void render_description(FILE *out, const plan_data_t *p) {
    if (p->description == NULL || p->description[0] == '\0') return;
    fprintf(out, "## Description\n\n%s\n\n---\n\n", p->description);
}

static void render_progress(FILE *out, sqlite3 *db, sqlite3_int64 plan_id) {
    /* Task counts per status. */
    const char *task_sql =
        "SELECT status, COUNT(*) FROM tasks WHERE plan_id = ? GROUP BY status;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, task_sql, -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(stmt, 1, plan_id);

    int cnt_done = 0, cnt_in_progress = 0, cnt_blocked = 0;
    int cnt_todo = 0, cnt_deferred = 0,    cnt_canceled = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *st = (const char *)sqlite3_column_text(stmt, 0);
        int n = sqlite3_column_int(stmt, 1);
        if (st == NULL) continue;
        if      (strcmp(st, "done")        == 0) cnt_done        += n;
        else if (strcmp(st, "in_progress") == 0) cnt_in_progress += n;
        else if (strcmp(st, "blocked")     == 0) cnt_blocked     += n;
        else if (strcmp(st, "todo")        == 0) cnt_todo        += n;
        else if (strcmp(st, "deferred")    == 0) cnt_deferred    += n;
        else if (strcmp(st, "canceled")    == 0) cnt_canceled    += n;
    }
    sqlite3_finalize(stmt);

    int total = cnt_done + cnt_in_progress + cnt_blocked +
                cnt_todo + cnt_deferred    + cnt_canceled;
    int pct   = (total > 0) ? (cnt_done * 100 / total) : 0;

    /* Phase counts per status. */
    const char *phase_sql =
        "SELECT status, COUNT(*) FROM phases WHERE plan_id = ? GROUP BY status;";
    if (sqlite3_prepare_v2(db, phase_sql, -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(stmt, 1, plan_id);

    int ph_open = 0, ph_in_progress = 0, ph_blocked = 0;
    int ph_completed = 0, ph_canceled = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *st = (const char *)sqlite3_column_text(stmt, 0);
        int n = sqlite3_column_int(stmt, 1);
        if (st == NULL) continue;
        if      (strcmp(st, "open")        == 0) ph_open        += n;
        else if (strcmp(st, "in_progress") == 0) ph_in_progress += n;
        else if (strcmp(st, "blocked")     == 0) ph_blocked     += n;
        else if (strcmp(st, "completed")   == 0) ph_completed   += n;
        else if (strcmp(st, "canceled")    == 0) ph_canceled    += n;
    }
    sqlite3_finalize(stmt);
    int ph_total = ph_open + ph_in_progress + ph_blocked +
                   ph_completed + ph_canceled;

    fprintf(out, "## Progress\n\n");

    /* Progress bar line. */
    fprintf(out, "**Completion:** ");
    render_progress_bar(out, cnt_done, total);
    fprintf(out, " **%d%%** (%d of %d tasks done)\n\n", pct, cnt_done, total);

    /* Task status breakdown table — only rows with non-zero counts. */
    fprintf(out, "| Status | Count |\n");
    fprintf(out, "|:--|--:|\n");
    if (cnt_done        > 0) fprintf(out, "| Done | %d |\n",        cnt_done);
    if (cnt_in_progress > 0) fprintf(out, "| In Progress | %d |\n", cnt_in_progress);
    if (cnt_blocked     > 0) fprintf(out, "| Blocked | %d |\n",     cnt_blocked);
    if (cnt_todo        > 0) fprintf(out, "| Todo | %d |\n",        cnt_todo);
    if (cnt_deferred    > 0) fprintf(out, "| Deferred | %d |\n",    cnt_deferred);
    if (cnt_canceled    > 0) fprintf(out, "| Canceled | %d |\n",    cnt_canceled);
    fprintf(out, "| **Total** | **%d** |\n\n", total);

    /* Phase summary line. */
    if (ph_total > 0) {
        fprintf(out, "**Phases:** %d total", ph_total);
        if (ph_completed   > 0) fprintf(out, " · %d completed",   ph_completed);
        if (ph_in_progress > 0) fprintf(out, " · %d in progress", ph_in_progress);
        if (ph_blocked     > 0) fprintf(out, " · %d blocked",     ph_blocked);
        if (ph_open        > 0) fprintf(out, " · %d open",        ph_open);
        if (ph_canceled    > 0) fprintf(out, " · %d canceled",    ph_canceled);
        fprintf(out, "\n\n");
    }

    fprintf(out, "---\n\n");
}

/* Emit one row of the task table for the statement currently on a row. */
static void render_task_row(FILE *out, sqlite3_stmt *stmt) {
    /* Columns: 0=id, 1=phase_id, 2=title, 3=status, 4=priority,
     *          5=task_type, 6=assignee, 7=local_seq, 8=uid        */
    const char *title    = (const char *)sqlite3_column_text(stmt, 2);
    const char *status   = (const char *)sqlite3_column_text(stmt, 3);
    const char *priority = (const char *)sqlite3_column_text(stmt, 4);
    const char *ttype    = (const char *)sqlite3_column_text(stmt, 5);
    const char *assignee = (const char *)sqlite3_column_text(stmt, 6);
    const char *uid      = (const char *)sqlite3_column_text(stmt, 8);

    fprintf(out, "| `%s` | %s | %s | %s | %s | %s |\n",
            uid ? uid : "—",
            title    ? title    : "—",
            label_task_type(ttype),
            label_priority(priority),
            (assignee && assignee[0] != '\0') ? assignee : "—",
            label_task_status(status));
}

/* Print a task table for tasks in a specific phase (or unassigned). */
static void render_task_table(FILE *out, sqlite3 *db,
                              sqlite3_int64 plan_id, sqlite3_int64 phase_id,
                              int unassigned) {
    const char *sql_phase =
        "SELECT id, phase_id, title, status, priority, task_type, assignee, "
        "local_seq, uid FROM tasks WHERE plan_id = ? AND phase_id = ? "
        "ORDER BY local_seq ASC;";
    const char *sql_unassigned =
        "SELECT id, phase_id, title, status, priority, task_type, assignee, "
        "local_seq, uid FROM tasks WHERE plan_id = ? AND phase_id IS NULL "
        "ORDER BY local_seq ASC;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, unassigned ? sql_unassigned : sql_phase,
                           -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(stmt, 1, plan_id);
    if (!unassigned) sqlite3_bind_int64(stmt, 2, phase_id);

    fprintf(out, "| ID | Title | Type | Priority | Assignee | Status |\n");
    fprintf(out, "|:---|:------|:-----|:---------|:---------|:-------|\n");

    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        render_task_row(out, stmt);
        count++;
    }
    sqlite3_finalize(stmt);

    if (count == 0) fprintf(out, "| — | *No tasks yet* | | | | |\n");
    fprintf(out, "\n");
}

static void render_phases(FILE *out, sqlite3 *db, sqlite3_int64 plan_id) {
    const char *sql =
        "SELECT id, title, summary, description, status, sequence_no, "
        "owner, target_start_date, target_end_date, local_seq "
        "FROM phases WHERE plan_id = ? ORDER BY sequence_no ASC;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(stmt, 1, plan_id);

    fprintf(out, "## Implementation Phases\n\n");

    int phase_count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        phase_count++;
        sqlite3_int64 phase_id  = sqlite3_column_int64(stmt, 0);
        const char *title       = (const char *)sqlite3_column_text(stmt, 1);
        const char *summary     = (const char *)sqlite3_column_text(stmt, 2);
        const char *description = (const char *)sqlite3_column_text(stmt, 3);
        const char *status      = (const char *)sqlite3_column_text(stmt, 4);
        sqlite3_int64 seq_no    = sqlite3_column_int64(stmt, 5);
        const char *owner       = (const char *)sqlite3_column_text(stmt, 6);
        const char *t_start     = (const char *)sqlite3_column_text(stmt, 7);
        const char *t_end       = (const char *)sqlite3_column_text(stmt, 8);

        /* Count tasks for this phase. */
        sqlite3_stmt *cnt = NULL;
        int task_total = 0, task_done = 0;
        const char *cnt_sql =
            "SELECT COUNT(*), "
            "SUM(CASE WHEN status = 'done' THEN 1 ELSE 0 END) "
            "FROM tasks WHERE phase_id = ?;";
        if (sqlite3_prepare_v2(db, cnt_sql, -1, &cnt, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(cnt, 1, phase_id);
            if (sqlite3_step(cnt) == SQLITE_ROW) {
                task_total = sqlite3_column_int(cnt, 0);
                task_done  = sqlite3_column_type(cnt, 1) != SQLITE_NULL
                             ? sqlite3_column_int(cnt, 1) : 0;
            }
            sqlite3_finalize(cnt);
        }

        char date_start[64], date_end[64];
        format_date(t_start, date_start, sizeof date_start);
        format_date(t_end,   date_end,   sizeof date_end);

        /* Phase heading. */
        fprintf(out, "### Phase %lld: %s\n\n",
                (long long)seq_no, title ? title : "Untitled");

        /* Phase metadata — trailing two spaces force a line break in Markdown. */
        fprintf(out, "**Status:** %s", label_phase_status(status));
        if (task_total > 0)
            fprintf(out, " · **Progress:** %d / %d", task_done, task_total);
        fprintf(out, "  \n");

        int has_dates = (t_start && t_start[0] != '\0') ||
                        (t_end   && t_end[0]   != '\0');
        if (has_dates) {
            fprintf(out, "**Dates:** ");
            fputs((t_start && t_start[0] != '\0') ? date_start : "—", out);
            fputs(" – ", out);
            fputs((t_end   && t_end[0]   != '\0') ? date_end   : "—", out);
            if (owner && owner[0] != '\0')
                fprintf(out, " · **Owner:** %s", owner);
            fprintf(out, "  \n");
        } else if (owner && owner[0] != '\0') {
            fprintf(out, "**Owner:** %s  \n", owner);
        }
        fprintf(out, "\n");

        if (summary && summary[0] != '\0')
            fprintf(out, "> %s\n\n", summary);
        if (description && description[0] != '\0')
            fprintf(out, "%s\n\n", description);

        render_task_table(out, db, plan_id, phase_id, 0);
        fprintf(out, "---\n\n");
    }
    sqlite3_finalize(stmt);

    /* Unassigned tasks section. */
    const char *unassigned_sql =
        "SELECT COUNT(*) FROM tasks WHERE plan_id = ? AND phase_id IS NULL;";
    sqlite3_stmt *ck = NULL;
    int unassigned_count = 0;
    if (sqlite3_prepare_v2(db, unassigned_sql, -1, &ck, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(ck, 1, plan_id);
        if (sqlite3_step(ck) == SQLITE_ROW)
            unassigned_count = sqlite3_column_int(ck, 0);
        sqlite3_finalize(ck);
    }
    if (unassigned_count > 0) {
        fprintf(out, "### Unassigned Tasks\n\n");
        fprintf(out, "*Tasks not yet associated with a phase.*\n\n");
        render_task_table(out, db, plan_id, 0, 1);
        fprintf(out, "---\n\n");
    }

    if (phase_count == 0 && unassigned_count == 0)
        fprintf(out, "*No phases or tasks defined yet.*\n\n---\n\n");
}

static void render_comments(FILE *out, sqlite3 *db, sqlite3_int64 plan_id) {
    const char *sql =
        "SELECT body, author, created_at, entity_type "
        "FROM comments "
        "WHERE invalidated_at IS NULL AND ("
        "  (entity_type = 'plan'  AND entity_id = ?1) OR"
        "  (entity_type = 'phase' AND entity_id IN "
        "     (SELECT id FROM phases WHERE plan_id = ?1)) OR"
        "  (entity_type = 'task'  AND entity_id IN "
        "     (SELECT id FROM tasks  WHERE plan_id = ?1))"
        ") ORDER BY id ASC;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(stmt, 1, plan_id);

    int first = 1;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (first) { fprintf(out, "## Notes\n\n"); first = 0; }
        const char *body       = (const char *)sqlite3_column_text(stmt, 0);
        const char *author     = (const char *)sqlite3_column_text(stmt, 1);
        const char *created_at = (const char *)sqlite3_column_text(stmt, 2);

        char date_str[64];
        format_date(created_at, date_str, sizeof date_str);

        fprintf(out, "> **%s** · %s  \n", author ? author : "—", date_str);
        fprintf(out, "> %s\n\n", body ? body : "");
    }
    sqlite3_finalize(stmt);

    if (!first) fprintf(out, "---\n\n");
}

static void render_closure(FILE *out, sqlite3 *db, sqlite3_int64 plan_id) {
    const char *sql =
        "SELECT resolution, outcome_summary, closing_comment, "
        "lessons_learned, open_items_summary, followup_needed, "
        "created_at, author "
        "FROM closure_records "
        "WHERE entity_type = 'plan' AND entity_id = ? "
        "ORDER BY id DESC LIMIT 1;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(stmt, 1, plan_id);
    if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); return; }

    const char *resolution  = (const char *)sqlite3_column_text(stmt, 0);
    const char *outcome     = (const char *)sqlite3_column_text(stmt, 1);
    const char *closing_cmt = (const char *)sqlite3_column_text(stmt, 2);
    const char *lessons     = (const char *)sqlite3_column_text(stmt, 3);
    const char *open_items  = (const char *)sqlite3_column_text(stmt, 4);
    int         followup    = sqlite3_column_int(stmt, 5);
    const char *created_at  = (const char *)sqlite3_column_text(stmt, 6);
    const char *author      = (const char *)sqlite3_column_text(stmt, 7);

    char date_closed[64];
    format_date(created_at, date_closed, sizeof date_closed);

    fprintf(out, "## Closure\n\n");
    fprintf(out, "**Resolution:** %s · **Closed:** %s",
            resolution ? resolution : "—", date_closed);
    if (author && author[0] != '\0')
        fprintf(out, " · **Author:** %s", author);
    fprintf(out, "\n\n");

    if (outcome && outcome[0] != '\0')
        fprintf(out, "**Outcome Summary**\n\n%s\n\n", outcome);
    if (closing_cmt && closing_cmt[0] != '\0')
        fprintf(out, "**Closing Notes**\n\n%s\n\n", closing_cmt);
    if (lessons && lessons[0] != '\0')
        fprintf(out, "**Lessons Learned**\n\n%s\n\n", lessons);
    if (open_items && open_items[0] != '\0')
        fprintf(out, "**Open Items**\n\n%s\n\n", open_items);
    if (followup)
        fprintf(out, "> **Note:** Follow-up action required for this plan.\n\n");

    sqlite3_finalize(stmt);
    fprintf(out, "---\n\n");
}

static void render_footer(FILE *out) {
    char today[64];
    format_today(today, sizeof today);
    fprintf(out, "*Generated by ipman · %s*\n", today);
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

int ipman_render_md(sqlite3 *db, sqlite3_int64 plan_id, FILE *out) {
    plan_data_t p;
    if (load_plan_data(db, plan_id, &p) != 0) {
        fprintf(stderr, "ipman render: plan not found (id=%lld)\n",
                (long long)plan_id);
        return -1;
    }

    render_header(out, &p);
    render_overview(out, &p);
    render_description(out, &p);
    render_progress(out, db, plan_id);
    render_phases(out, db, plan_id);
    render_comments(out, db, plan_id);
    render_closure(out, db, plan_id);
    render_footer(out);

    plan_data_free(&p);
    return 0;
}
