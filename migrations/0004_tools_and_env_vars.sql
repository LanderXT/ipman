-- Migration 0004: project entity + project requirements registry
--
-- Introduces a workspace-level "project" entity that captures what this
-- repository / IPMAN_HOME *is* (name and description) and what it *needs*
-- (tools and env_vars). Three tables:
--
--   * `project`  — single-row table (CHECK id = 1) with the project's name
--                  and description. The row is inserted by this migration
--                  with name='unnamed'; agents update it via project.update.
--   * `tools`    — required executables/runtimes. One row per tool name,
--                  enforced via UNIQUE INDEX on the active row only.
--                  Plan-agnostic: tools belong to the project, not to a
--                  particular plan.
--   * `env_vars` — required environment variables. ipman is a registry of
--                  requirements; there is intentionally no `value` column.
--                  When `sensitive = 1`, callers must mask `example` unless
--                  the request explicitly opts in (`reveal = true`).
--
-- Both tools and env_vars follow the existing soft-delete (`invalidated_at/by`)
-- and audit (`created_at/updated_at`) conventions used by `instructions`.
--
-- Seven new event_types are introduced (project_updated; tool_added/updated/
-- invalidated; env_var_added/updated/invalidated). The events table is also
-- extended to accept entity_type='project' for these workspace-level rows.
-- Because SQLite cannot ALTER a CHECK constraint, the events table is
-- rebuilt — same pattern used in 0001 to evolve the allowlist for
-- instructions.

CREATE TABLE IF NOT EXISTS project (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    name TEXT NOT NULL CHECK (length(trim(name)) > 0),
    description TEXT
        CHECK (description IS NULL OR length(trim(description)) > 0),
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at TEXT
);

INSERT OR IGNORE INTO project(id, name) VALUES (1, 'unnamed');

CREATE TABLE IF NOT EXISTS tools (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL CHECK (length(trim(name)) > 0),
    version_constraint TEXT
        CHECK (version_constraint IS NULL OR length(trim(version_constraint)) > 0),
    purpose TEXT
        CHECK (purpose IS NULL OR length(trim(purpose)) > 0),
    install_hint TEXT
        CHECK (install_hint IS NULL OR length(trim(install_hint)) > 0),
    required INTEGER NOT NULL DEFAULT 1 CHECK (required IN (0, 1)),
    author TEXT NOT NULL CHECK (length(trim(author)) > 0),
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at TEXT,
    invalidated_at TEXT,
    invalidated_by TEXT
);

CREATE INDEX IF NOT EXISTS idx_tools_created
    ON tools(created_at);
CREATE UNIQUE INDEX IF NOT EXISTS idx_tools_name_active
    ON tools(name) WHERE invalidated_at IS NULL;

CREATE TABLE IF NOT EXISTS env_vars (
    id INTEGER PRIMARY KEY,
    -- UPPER_SNAKE_CASE, may contain digits but must not start with one.
    -- The full regex `^[A-Z_][A-Z0-9_]*$` is enforced at the op layer; the
    -- GLOB below is a coarse defence-in-depth against direct SQL writes.
    name TEXT NOT NULL
        CHECK (length(trim(name)) > 0
               AND name GLOB '[A-Z_]*'
               AND name NOT GLOB '*[^A-Z0-9_]*'),
    purpose TEXT NOT NULL CHECK (length(trim(purpose)) > 0),
    example TEXT
        CHECK (example IS NULL OR length(trim(example)) > 0),
    required INTEGER NOT NULL DEFAULT 1 CHECK (required IN (0, 1)),
    sensitive INTEGER NOT NULL DEFAULT 0 CHECK (sensitive IN (0, 1)),
    author TEXT NOT NULL CHECK (length(trim(author)) > 0),
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at TEXT,
    invalidated_at TEXT,
    invalidated_by TEXT
);

CREATE INDEX IF NOT EXISTS idx_env_vars_created
    ON env_vars(created_at);
CREATE UNIQUE INDEX IF NOT EXISTS idx_env_vars_name_active
    ON env_vars(name) WHERE invalidated_at IS NULL;

-- Extend the events allowlist with the new entity_type ('project') and the
-- seven new event_types. Rebuild the table because the CHECK on entity_type
-- and event_type cannot be altered in place. Schema is otherwise unchanged.

CREATE TABLE events_new (
    id INTEGER PRIMARY KEY,
    entity_type TEXT NOT NULL
        CHECK (entity_type IN ('plan','phase','task','project')),
    entity_id INTEGER NOT NULL CHECK (entity_id > 0),
    event_type TEXT NOT NULL CHECK (event_type IN (
        'plan_created','plan_updated','plan_closed','plan_archived',
        'plan_reopened','plan_activated','plan_deactivated',
        'phase_created','phase_updated','phase_moved','phase_closed',
        'phase_reopened','phase_current_changed','task_created',
        'task_updated','task_linked_external','task_status_changed',
        'task_deferred','task_canceled','task_replaced','task_reopened',
        'task_closed','task_current_changed','comment_added',
        'comment_updated','comment_invalidated','instruction_added',
        'instruction_updated','instruction_invalidated',
        'project_updated',
        'tool_added','tool_updated','tool_invalidated',
        'env_var_added','env_var_updated','env_var_invalidated'
    )),
    actor TEXT NOT NULL CHECK (length(trim(actor)) > 0),
    event_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    summary TEXT NOT NULL,
    details TEXT,
    old_value TEXT,
    new_value TEXT,
    related_entity_type TEXT
        CHECK (related_entity_type IS NULL OR
               related_entity_type IN ('plan','phase','task','project')),
    related_entity_id INTEGER
        CHECK (related_entity_id IS NULL OR related_entity_id > 0),
    request_id TEXT
);

INSERT INTO events_new(
    id, entity_type, entity_id, event_type, actor, event_at, summary, details,
    old_value, new_value, related_entity_type, related_entity_id, request_id
)
SELECT
    id, entity_type, entity_id, event_type, actor, event_at, summary, details,
    old_value, new_value, related_entity_type, related_entity_id, request_id
FROM events;

DROP TABLE events;
ALTER TABLE events_new RENAME TO events;

CREATE INDEX IF NOT EXISTS idx_events_entity
    ON events(entity_type, entity_id, event_at);
CREATE INDEX IF NOT EXISTS idx_events_event_at ON events(event_at);
CREATE INDEX IF NOT EXISTS idx_events_type_at ON events(event_type, event_at);
