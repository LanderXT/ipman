-- Migration 0006: per-branch workspace context
--
-- branch_contexts maps a git branch name to the plan that should be
-- active when the user is on that branch. plan.activate upserts a row
-- here when called inside a git repo; workspace.context_get reads it
-- to auto-select the active plan by branch.
--
-- ON DELETE RESTRICT on active_plan_id: branches can outlive a plan
-- (the plan may be closed), so we keep stale bindings rather than
-- silently deleting them. context_get skips terminal plans and falls
-- back to the global workspace_context.
CREATE TABLE IF NOT EXISTS branch_contexts (
    branch_name    TEXT PRIMARY KEY CHECK (length(trim(branch_name)) > 0),
    active_plan_id INTEGER NOT NULL,
    updated_at     TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_by     TEXT NOT NULL CHECK (length(trim(updated_by)) > 0),
    FOREIGN KEY (active_plan_id) REFERENCES plans(id) ON DELETE RESTRICT
);
