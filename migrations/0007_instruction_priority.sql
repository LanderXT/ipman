-- Migration 0007: priority field for project-scoped instructions
--
-- Adds a `priority` column to `instructions` with values 'critical' or
-- 'normal' (default 'normal'). The CHECK constraint requires a table
-- rebuild because SQLite cannot ALTER an existing CHECK in place.
--
-- The column is present on all rows for schema simplicity; the op layer
-- enforces that only project-scoped instructions (entity_type='project')
-- may supply a non-default value. Extending priority to other scopes
-- later requires only removing that op-layer guard.

CREATE TABLE instructions_new (
    id INTEGER PRIMARY KEY,
    entity_type TEXT NOT NULL
        CHECK (entity_type IN ('plan','phase','task','project')),
    entity_id INTEGER NOT NULL CHECK (entity_id > 0),
    instruction_type TEXT NOT NULL DEFAULT 'guidance'
        CHECK (length(trim(instruction_type)) > 0),
    priority TEXT NOT NULL DEFAULT 'normal'
        CHECK (priority IN ('critical','normal')),
    body TEXT NOT NULL CHECK (length(trim(body)) > 0),
    author TEXT NOT NULL CHECK (length(trim(author)) > 0),
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at TEXT,
    invalidated_at TEXT,
    invalidated_by TEXT
);

INSERT INTO instructions_new(
    id, entity_type, entity_id, instruction_type, priority, body, author,
    created_at, updated_at, invalidated_at, invalidated_by
)
SELECT id, entity_type, entity_id, instruction_type, 'normal', body, author,
       created_at, updated_at, invalidated_at, invalidated_by
FROM instructions;

DROP TABLE instructions;
ALTER TABLE instructions_new RENAME TO instructions;

CREATE INDEX IF NOT EXISTS idx_instructions_entity
    ON instructions(entity_type, entity_id, created_at);
