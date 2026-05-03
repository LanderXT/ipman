-- Migration 0005: instructions can attach to the project
--
-- Extends `instructions.entity_type` to accept 'project' alongside
-- plan/phase/task. Project-scoped instructions describe repository-wide
-- standing guidance ("Apache-2.0 in headers", "never commit secrets",
-- "all tests must pass before merging") that should survive plan
-- lifecycle and surface at session start regardless of which plan, if
-- any, is active.
--
-- entity_id must be 1 when entity_type='project' (the project table is
-- single-row by construction). The CHECK below cannot enforce that
-- specific value without coupling to the single-row invariant of
-- another table; the constraint is enforced at the op layer instead.
--
-- Because SQLite cannot ALTER a CHECK constraint, the table is rebuilt.
-- The events allowlist already accepts entity_type='project' from
-- migration 0004, so instruction_added/updated/invalidated events emitted
-- against the project will land without further schema changes.

CREATE TABLE instructions_new (
    id INTEGER PRIMARY KEY,
    entity_type TEXT NOT NULL
        CHECK (entity_type IN ('plan','phase','task','project')),
    entity_id INTEGER NOT NULL CHECK (entity_id > 0),
    instruction_type TEXT NOT NULL DEFAULT 'guidance'
        CHECK (length(trim(instruction_type)) > 0),
    body TEXT NOT NULL CHECK (length(trim(body)) > 0),
    author TEXT NOT NULL CHECK (length(trim(author)) > 0),
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at TEXT,
    invalidated_at TEXT,
    invalidated_by TEXT
);

INSERT INTO instructions_new(
    id, entity_type, entity_id, instruction_type, body, author,
    created_at, updated_at, invalidated_at, invalidated_by
)
SELECT id, entity_type, entity_id, instruction_type, body, author,
       created_at, updated_at, invalidated_at, invalidated_by
FROM instructions;

DROP TABLE instructions;
ALTER TABLE instructions_new RENAME TO instructions;

CREATE INDEX IF NOT EXISTS idx_instructions_entity
    ON instructions(entity_type, entity_id, created_at);
