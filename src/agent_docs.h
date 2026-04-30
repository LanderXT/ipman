#ifndef IPMAN_AGENT_DOCS_H
#define IPMAN_AGENT_DOCS_H

#include "dispatch.h"

/*
 * Stats from a single agent-docs refresh. Only populated by callers that
 * expose the numbers downstream (today: workspace.refresh_agent_docs,
 * which surfaces them in its response). Callers that just want the docs
 * regenerated and don't care about the audit (e.g. `ipman init`) pass NULL
 * for `result_out` and ignore this struct entirely.
 */
typedef struct {
    int files_total;
    int files_written;
    int files_unchanged;
    char generated_at[32];
    char source_fingerprint[32];
    char content_fingerprint[32];
} ipman_agent_docs_result_t;

/*
 * Regenerate the .ipman/ agent-docs tree under `home` (manifest, indexes,
 * per-op schemas, etc.) from the live SQL migrations + dispatch table.
 * `db_path` is consulted for schema-version metadata only — the function
 * does not modify the database.
 *
 * `result_out` is OPTIONAL: pass a non-NULL struct to receive the file
 * counts and content/source fingerprints, or NULL to discard them. The
 * `ipman init` path passes NULL because nothing downstream surfaces the
 * stats; the workspace.refresh_agent_docs handler passes a real struct
 * because its JSON response includes them.
 *
 * Returns 0 on success, non-zero on failure (with a diagnostic on stderr).
 */
int ipman_agent_docs_refresh(const char *home,
                            const char *db_path,
                            ipman_agent_docs_result_t *result_out);

int ipman_agent_docs_operation_spec_count(void);
const char *ipman_agent_docs_operation_spec_name_at(int index);

int ipman_op_workspace_refresh_agent_docs(const ipman_request_t *req,
                                         sqlite3 *db,
                                         cJSON **result_out,
                                         ipman_error_code_t *err_code_out,
                                         const char **err_msg_out);
extern const ipman_param_desc_t ipman_op_workspace_refresh_agent_docs_params[];

#endif
