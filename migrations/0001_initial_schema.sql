-- Migration 0001: initial schema (ipman v1 public release)
-- Consolidated from prior incremental migrations.


-- ===== source: 0001_schema_metadata.sql =====
-- Migration 0001: schema_metadata
--
-- Bootstrap table that records every migration the runner has applied.
-- Must be idempotent: any re-run of this file against an already-initialised
-- DB must be a no-op.

CREATE TABLE IF NOT EXISTS schema_metadata (
    version     INTEGER PRIMARY KEY,
    name        TEXT    NOT NULL,
    applied_at  TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    checksum    TEXT    NOT NULL
);

-- ===== source: 0002_plans_and_events.sql =====
-- Migration 0002: plans and events
--
-- Phase 4 vertical slice: enough domain schema to create a Plan and record
-- its creation event in the same transaction.

CREATE TABLE IF NOT EXISTS plans (
    id             INTEGER PRIMARY KEY,
    code           TEXT UNIQUE,
    title          TEXT NOT NULL CHECK (length(trim(title)) > 0),
    summary        TEXT,
    description    TEXT,
    status         TEXT NOT NULL DEFAULT 'open'
                   CHECK (status IN (
                       'open', 'in_progress', 'paused',
                       'completed', 'canceled', 'archived'
                   )),
    priority       TEXT
                   CHECK (priority IS NULL OR priority IN (
                       'low', 'medium', 'high', 'critical'
                   )),
    created_at     TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at     TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    opened_at      TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    closed_at      TEXT,
    archived_at    TEXT,
    owner          TEXT,
    target_date    TEXT,
    tags           TEXT,
    version_label  TEXT
);

CREATE TABLE IF NOT EXISTS events (
    id                   INTEGER PRIMARY KEY,
    entity_type          TEXT NOT NULL
                         CHECK (entity_type IN ('plan', 'phase', 'task')),
    entity_id            INTEGER NOT NULL CHECK (entity_id > 0),
    event_type           TEXT NOT NULL
                         CHECK (event_type IN (
                             'plan_created',
                             'phase_created',
                             'task_created',
                             'task_updated',
                             'task_status_changed',
                             'task_deferred',
                             'task_canceled',
                             'task_replaced',
                             'task_reopened',
                             'comment_added',
                             'phase_closed',
                             'task_closed'
                         )),
    actor                TEXT NOT NULL CHECK (length(trim(actor)) > 0),
    event_at             TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    summary              TEXT NOT NULL,
    details              TEXT,
    old_value            TEXT,
    new_value            TEXT,
    related_entity_type  TEXT
                         CHECK (
                             related_entity_type IS NULL OR
                             related_entity_type IN ('plan', 'phase', 'task')
                         ),
    related_entity_id    INTEGER CHECK (
                             related_entity_id IS NULL OR related_entity_id > 0
                         ),
    request_id           TEXT
);

-- ===== source: 0003_phases.sql =====
-- Migration 0003: phases
--
-- Phase rows belong to a plan and carry an explicit per-plan sequence
-- number. Reordering policy is left to phase.move, but duplicate positions
-- inside one plan are rejected at the schema level.

CREATE TABLE IF NOT EXISTS phases (
    id                 INTEGER PRIMARY KEY,
    plan_id            INTEGER NOT NULL,
    title              TEXT NOT NULL CHECK (length(trim(title)) > 0),
    summary            TEXT,
    description        TEXT,
    status             TEXT NOT NULL DEFAULT 'open'
                       CHECK (status IN (
                           'open', 'in_progress', 'blocked',
                           'completed', 'canceled'
                       )),
    sequence_no        INTEGER NOT NULL CHECK (sequence_no > 0),
    created_at         TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at         TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    opened_at          TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    closed_at          TEXT,
    owner              TEXT,
    target_start_date  TEXT,
    target_end_date    TEXT,

    FOREIGN KEY (plan_id) REFERENCES plans(id) ON DELETE RESTRICT,
    UNIQUE (plan_id, sequence_no),
    UNIQUE (id, plan_id)
);

-- ===== source: 0004_tasks.sql =====
-- Migration 0004: tasks
--
-- Tasks intentionally omit replacement_task_id. Replacement links are stored
-- in task_relations; origin_task_id remains origin metadata for the new task.

CREATE TABLE IF NOT EXISTS tasks (
    id                 INTEGER PRIMARY KEY,
    plan_id            INTEGER NOT NULL,
    phase_id           INTEGER,
    parent_task_id     INTEGER,
    title              TEXT NOT NULL CHECK (length(trim(title)) > 0),
    summary            TEXT,
    description        TEXT,
    status             TEXT NOT NULL DEFAULT 'todo'
                       CHECK (status IN (
                           'todo', 'in_progress', 'blocked',
                           'deferred', 'done', 'canceled'
                       )),
    resolution         TEXT
                       CHECK (resolution IS NULL OR resolution IN (
                           'completed', 'canceled', 'not_planned',
                           'replaced', 'duplicate', 'discarded'
                       )),
    priority           TEXT NOT NULL DEFAULT 'medium'
                       CHECK (priority IN (
                           'low', 'medium', 'high', 'critical'
                       )),
    task_type          TEXT NOT NULL DEFAULT 'task'
                       CHECK (task_type IN (
                           'task', 'research', 'bug', 'decision',
                           'review', 'documentation'
                       )),
    origin_type        TEXT NOT NULL DEFAULT 'planned'
                       CHECK (origin_type IN (
                           'planned', 'addendum', 'discovered',
                           'replacement', 'carryover', 'external_request'
                       )),
    assignee           TEXT,
    created_at         TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at         TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    started_at         TEXT,
    closed_at          TEXT,

    deferred_until     TEXT,
    blocked_reason     TEXT,
    reason_code        TEXT,
    reason_text        TEXT,
    due_date           TEXT,
    target_start_date  TEXT,
    estimate           TEXT,

    origin_ref_type    TEXT,
    origin_ref_id      TEXT,
    origin_task_id     INTEGER,

    FOREIGN KEY (plan_id) REFERENCES plans(id) ON DELETE RESTRICT,
    FOREIGN KEY (phase_id, plan_id)
        REFERENCES phases(id, plan_id) ON DELETE RESTRICT,
    FOREIGN KEY (parent_task_id, plan_id)
        REFERENCES tasks(id, plan_id) ON DELETE RESTRICT,
    FOREIGN KEY (origin_task_id) REFERENCES tasks(id) ON DELETE RESTRICT,
    UNIQUE (id, plan_id)
);

-- ===== source: 0005_comments.sql =====
-- Migration 0005: comments
--
-- Comments are polymorphic across the main entity types. entity_id cannot be
-- a real FK because the parent table depends on entity_type.

CREATE TABLE IF NOT EXISTS comments (
    id            INTEGER PRIMARY KEY,
    entity_type   TEXT NOT NULL
                  CHECK (entity_type IN ('plan', 'phase', 'task')),
    entity_id     INTEGER NOT NULL CHECK (entity_id > 0),
    comment_type  TEXT NOT NULL DEFAULT 'general',
    body          TEXT NOT NULL CHECK (length(trim(body)) > 0),
    author        TEXT NOT NULL CHECK (length(trim(author)) > 0),
    created_at    TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at    TEXT
);

-- ===== source: 0006_closure_records.sql =====
-- Migration 0006: closure records
--
-- A closure record stores the memory of closing a task, phase, or plan and is
-- linked to the event that created it.

CREATE TABLE IF NOT EXISTS closure_records (
    id                  INTEGER PRIMARY KEY,
    entity_type         TEXT NOT NULL
                        CHECK (entity_type IN ('plan', 'phase', 'task')),
    entity_id           INTEGER NOT NULL CHECK (entity_id > 0),
    closure_status      TEXT NOT NULL CHECK (length(trim(closure_status)) > 0),
    resolution          TEXT
                        CHECK (resolution IS NULL OR resolution IN (
                            'completed', 'canceled', 'not_planned',
                            'replaced', 'duplicate', 'discarded'
                        )),
    outcome_summary     TEXT NOT NULL CHECK (length(trim(outcome_summary)) > 0),
    closing_comment     TEXT NOT NULL CHECK (length(trim(closing_comment)) > 0),
    lessons_learned     TEXT,
    open_items_summary  TEXT,
    followup_needed     INTEGER NOT NULL DEFAULT 0
                        CHECK (followup_needed IN (0, 1)),
    created_at          TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    author              TEXT NOT NULL CHECK (length(trim(author)) > 0),
    event_id            INTEGER NOT NULL,

    FOREIGN KEY (event_id) REFERENCES events(id) ON DELETE RESTRICT
);

-- ===== source: 0007_task_relations.sql =====
-- Migration 0007: task relations
--
-- This table is the single source of truth for replacement links and other
-- task-to-task relations.

CREATE TABLE IF NOT EXISTS task_relations (
    id             INTEGER PRIMARY KEY,
    from_task_id   INTEGER NOT NULL,
    to_task_id     INTEGER NOT NULL,
    relation_type  TEXT NOT NULL
                   CHECK (relation_type IN (
                       'blocks', 'blocked_by', 'related',
                       'duplicates', 'replaces'
                   )),
    created_at     TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    created_by     TEXT NOT NULL CHECK (length(trim(created_by)) > 0),
    notes          TEXT,

    FOREIGN KEY (from_task_id) REFERENCES tasks(id) ON DELETE RESTRICT,
    FOREIGN KEY (to_task_id) REFERENCES tasks(id) ON DELETE RESTRICT,
    CHECK (from_task_id <> to_task_id),
    UNIQUE (from_task_id, to_task_id, relation_type)
);

-- ===== source: 0008_indexes.sql =====
-- Migration 0008: query indexes
--
-- Indexes for the minimum operational queries listed in D§9.

CREATE INDEX IF NOT EXISTS idx_tasks_plan
    ON tasks(plan_id);

CREATE INDEX IF NOT EXISTS idx_tasks_phase
    ON tasks(phase_id)
    WHERE phase_id IS NOT NULL;

CREATE INDEX IF NOT EXISTS idx_tasks_status
    ON tasks(status);

CREATE INDEX IF NOT EXISTS idx_tasks_resolution
    ON tasks(resolution)
    WHERE resolution IS NOT NULL;

CREATE INDEX IF NOT EXISTS idx_tasks_deferred_until
    ON tasks(deferred_until)
    WHERE deferred_until IS NOT NULL;

CREATE INDEX IF NOT EXISTS idx_tasks_origin
    ON tasks(origin_type);

CREATE INDEX IF NOT EXISTS idx_tasks_assignee
    ON tasks(assignee)
    WHERE assignee IS NOT NULL;

CREATE INDEX IF NOT EXISTS idx_tasks_origin_task
    ON tasks(origin_task_id)
    WHERE origin_task_id IS NOT NULL;

CREATE INDEX IF NOT EXISTS idx_events_entity
    ON events(entity_type, entity_id, event_at);

CREATE INDEX IF NOT EXISTS idx_comments_entity
    ON comments(entity_type, entity_id, created_at);

CREATE INDEX IF NOT EXISTS idx_closure_records_entity
    ON closure_records(entity_type, entity_id, created_at);

CREATE INDEX IF NOT EXISTS idx_task_relations_from
    ON task_relations(from_task_id, relation_type);

CREATE INDEX IF NOT EXISTS idx_task_relations_to
    ON task_relations(to_task_id, relation_type);

-- ===== source: 0009_plan_event_types.sql =====
-- Migration 0009: plan operation event types
--
-- Phase 6 adds semantic plan operations. The original events CHECK only
-- allowed plan_created, so widen the catalog while preserving existing rows.

CREATE TABLE IF NOT EXISTS events_new (
    id                   INTEGER PRIMARY KEY,
    entity_type          TEXT NOT NULL
                         CHECK (entity_type IN ('plan', 'phase', 'task')),
    entity_id            INTEGER NOT NULL CHECK (entity_id > 0),
    event_type           TEXT NOT NULL
                         CHECK (event_type IN (
                             'plan_created',
                             'plan_updated',
                             'plan_closed',
                             'plan_archived',
                             'plan_reopened',
                             'phase_created',
                             'task_created',
                             'task_updated',
                             'task_status_changed',
                             'task_deferred',
                             'task_canceled',
                             'task_replaced',
                             'task_reopened',
                             'comment_added',
                             'phase_closed',
                             'task_closed'
                         )),
    actor                TEXT NOT NULL CHECK (length(trim(actor)) > 0),
    event_at             TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    summary              TEXT NOT NULL,
    details              TEXT,
    old_value            TEXT,
    new_value            TEXT,
    related_entity_type  TEXT
                         CHECK (
                             related_entity_type IS NULL OR
                             related_entity_type IN ('plan', 'phase', 'task')
                         ),
    related_entity_id    INTEGER CHECK (
                             related_entity_id IS NULL OR related_entity_id > 0
                         ),
    request_id           TEXT
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

-- ===== source: 0010_phase_event_types.sql =====
-- Migration 0010: phase operation event types
--
-- Phase 7 adds semantic phase operations. Rebuild the events CHECK catalog
-- to allow the new phase update/move/reopen events.

CREATE TABLE IF NOT EXISTS events_new (
    id                   INTEGER PRIMARY KEY,
    entity_type          TEXT NOT NULL
                         CHECK (entity_type IN ('plan', 'phase', 'task')),
    entity_id            INTEGER NOT NULL CHECK (entity_id > 0),
    event_type           TEXT NOT NULL
                         CHECK (event_type IN (
                             'plan_created',
                             'plan_updated',
                             'plan_closed',
                             'plan_archived',
                             'plan_reopened',
                             'phase_created',
                             'phase_updated',
                             'phase_moved',
                             'phase_closed',
                             'phase_reopened',
                             'task_created',
                             'task_updated',
                             'task_status_changed',
                             'task_deferred',
                             'task_canceled',
                             'task_replaced',
                             'task_reopened',
                             'comment_added',
                             'task_closed'
                         )),
    actor                TEXT NOT NULL CHECK (length(trim(actor)) > 0),
    event_at             TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    summary              TEXT NOT NULL,
    details              TEXT,
    old_value            TEXT,
    new_value            TEXT,
    related_entity_type  TEXT
                         CHECK (
                             related_entity_type IS NULL OR
                             related_entity_type IN ('plan', 'phase', 'task')
                         ),
    related_entity_id    INTEGER CHECK (
                             related_entity_id IS NULL OR related_entity_id > 0
                         ),
    request_id           TEXT
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

-- ===== source: 0011_comment_lifecycle_event_types.sql =====
-- Migration 0011: comment lifecycle and event types
--
-- Phase 11 adds soft invalidation for comments and extends the event CHECK
-- catalog for comment update/invalidation audit rows.

ALTER TABLE comments ADD COLUMN invalidated_at TEXT;
ALTER TABLE comments ADD COLUMN invalidated_by TEXT;

CREATE TABLE IF NOT EXISTS events_new (
    id                   INTEGER PRIMARY KEY,
    entity_type          TEXT NOT NULL
                         CHECK (entity_type IN ('plan', 'phase', 'task')),
    entity_id            INTEGER NOT NULL CHECK (entity_id > 0),
    event_type           TEXT NOT NULL
                         CHECK (event_type IN (
                             'plan_created',
                             'plan_updated',
                             'plan_closed',
                             'plan_archived',
                             'plan_reopened',
                             'phase_created',
                             'phase_updated',
                             'phase_moved',
                             'phase_closed',
                             'phase_reopened',
                             'task_created',
                             'task_updated',
                             'task_status_changed',
                             'task_deferred',
                             'task_canceled',
                             'task_replaced',
                             'task_reopened',
                             'comment_added',
                             'comment_updated',
                             'comment_invalidated',
                             'task_closed'
                         )),
    actor                TEXT NOT NULL CHECK (length(trim(actor)) > 0),
    event_at             TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    summary              TEXT NOT NULL,
    details              TEXT,
    old_value            TEXT,
    new_value            TEXT,
    related_entity_type  TEXT
                         CHECK (
                             related_entity_type IS NULL OR
                             related_entity_type IN ('plan', 'phase', 'task')
                         ),
    related_entity_id    INTEGER CHECK (
                             related_entity_id IS NULL OR related_entity_id > 0
                         ),
    request_id           TEXT
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

-- ===== source: 0012_query_indexes.sql =====
-- Migration 0012: Phase 12 query indexes
--
-- Add indexes for query/listing surfaces introduced after the original
-- Phase 5 index pass.

CREATE INDEX IF NOT EXISTS idx_tasks_priority
    ON tasks(priority);

CREATE INDEX IF NOT EXISTS idx_phases_plan_status
    ON phases(plan_id, status);

CREATE INDEX IF NOT EXISTS idx_events_event_at
    ON events(event_at);

CREATE INDEX IF NOT EXISTS idx_events_type_at
    ON events(event_type, event_at);

-- ===== source: 0013_workspace_context.sql =====
-- Migration 0013: workspace and plan context

CREATE TABLE IF NOT EXISTS workspace_context (
    id              INTEGER PRIMARY KEY CHECK (id = 1),
    active_plan_id  INTEGER,
    updated_at      TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_by      TEXT NOT NULL CHECK (length(trim(updated_by)) > 0),

    FOREIGN KEY (active_plan_id) REFERENCES plans(id) ON DELETE RESTRICT
);

CREATE TABLE IF NOT EXISTS plan_contexts (
    plan_id           INTEGER PRIMARY KEY,
    current_phase_id  INTEGER,
    current_task_id   INTEGER,
    updated_at        TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_by        TEXT NOT NULL CHECK (length(trim(updated_by)) > 0),

    FOREIGN KEY (plan_id) REFERENCES plans(id) ON DELETE RESTRICT,
    FOREIGN KEY (current_phase_id, plan_id)
        REFERENCES phases(id, plan_id) ON DELETE RESTRICT,
    FOREIGN KEY (current_task_id, plan_id)
        REFERENCES tasks(id, plan_id) ON DELETE RESTRICT
);

CREATE TABLE IF NOT EXISTS events_new (
    id                   INTEGER PRIMARY KEY,
    entity_type          TEXT NOT NULL
                         CHECK (entity_type IN ('plan', 'phase', 'task')),
    entity_id            INTEGER NOT NULL CHECK (entity_id > 0),
    event_type           TEXT NOT NULL
                         CHECK (event_type IN (
                             'plan_created',
                             'plan_updated',
                             'plan_closed',
                             'plan_archived',
                             'plan_reopened',
                             'plan_activated',
                             'plan_deactivated',
                             'phase_created',
                             'phase_updated',
                             'phase_moved',
                             'phase_closed',
                             'phase_reopened',
                             'phase_current_changed',
                             'task_created',
                             'task_updated',
                             'task_status_changed',
                             'task_deferred',
                             'task_canceled',
                             'task_replaced',
                             'task_reopened',
                             'task_closed',
                             'task_current_changed',
                             'comment_added',
                             'comment_updated',
                             'comment_invalidated'
                         )),
    actor                TEXT NOT NULL CHECK (length(trim(actor)) > 0),
    event_at             TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    summary              TEXT NOT NULL,
    details              TEXT,
    old_value            TEXT,
    new_value            TEXT,
    related_entity_type  TEXT
                         CHECK (
                             related_entity_type IS NULL OR
                             related_entity_type IN ('plan', 'phase', 'task')
                         ),
    related_entity_id    INTEGER CHECK (
                             related_entity_id IS NULL OR related_entity_id > 0
                         ),
    request_id           TEXT
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

-- ===== source: 0014_task_link_external_event_type.sql =====
-- Migration 0014: task.link_external event type
--
-- Phase 14 adds a dedicated audit event when a task is linked to an
-- external artifact, so widen the events CHECK catalog accordingly.

CREATE TABLE IF NOT EXISTS events_new (
    id                   INTEGER PRIMARY KEY,
    entity_type          TEXT NOT NULL
                         CHECK (entity_type IN ('plan', 'phase', 'task')),
    entity_id            INTEGER NOT NULL CHECK (entity_id > 0),
    event_type           TEXT NOT NULL
                         CHECK (event_type IN (
                             'plan_created',
                             'plan_updated',
                             'plan_closed',
                             'plan_archived',
                             'plan_reopened',
                             'plan_activated',
                             'plan_deactivated',
                             'phase_created',
                             'phase_updated',
                             'phase_moved',
                             'phase_closed',
                             'phase_reopened',
                             'phase_current_changed',
                             'task_created',
                             'task_updated',
                             'task_linked_external',
                             'task_status_changed',
                             'task_deferred',
                             'task_canceled',
                             'task_replaced',
                             'task_reopened',
                             'task_closed',
                             'task_current_changed',
                             'comment_added',
                             'comment_updated',
                             'comment_invalidated'
                         )),
    actor                TEXT NOT NULL CHECK (length(trim(actor)) > 0),
    event_at             TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    summary              TEXT NOT NULL,
    details              TEXT,
    old_value            TEXT,
    new_value            TEXT,
    related_entity_type  TEXT
                         CHECK (
                             related_entity_type IS NULL OR
                             related_entity_type IN ('plan', 'phase', 'task')
                         ),
    related_entity_id    INTEGER CHECK (
                             related_entity_id IS NULL OR related_entity_id > 0
                         ),
    request_id           TEXT
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

-- ===== source: 0015_phases_local_seq.sql =====
-- Migration 0015: stable per-plan identity for phases.
--
-- `sequence_no` is the display order of phases within a plan; it is dense
-- (1..N) and gets rewritten by `phase.move`. A stable identifier is needed
-- so that references in closures, comments, and handoffs remain valid after
-- reordering. `local_seq` is that identifier: assigned in creation order
-- per plan and never reassigned.

ALTER TABLE phases ADD COLUMN local_seq INTEGER;

WITH ordered AS (
    SELECT id,
           ROW_NUMBER() OVER (PARTITION BY plan_id
                              ORDER BY created_at, id) AS rn
    FROM phases
)
UPDATE phases
   SET local_seq = (SELECT rn FROM ordered WHERE ordered.id = phases.id)
 WHERE local_seq IS NULL;

CREATE TRIGGER IF NOT EXISTS phases_fill_local_seq
AFTER INSERT ON phases
WHEN new.local_seq IS NULL
BEGIN
    UPDATE phases
       SET local_seq = COALESCE(
           (SELECT MAX(local_seq) + 1 FROM phases
             WHERE plan_id = new.plan_id AND id <> new.id),
           1)
     WHERE id = new.id;
END;

CREATE UNIQUE INDEX IF NOT EXISTS idx_phases_plan_local_seq
    ON phases(plan_id, local_seq);

-- ===== source: 0016_tasks_local_seq.sql =====
-- Migration 0016: stable per-plan identity for tasks.
--
-- Tasks had no per-plan identifier other than the global `id`. `local_seq`
-- is dense within each plan, assigned in creation order, and never
-- reassigned. Together with the plan code it forms a stable ref of the
-- shape `<plan_code>/T<local_seq>` suitable for comments, events, and
-- handoff notes.

ALTER TABLE tasks ADD COLUMN local_seq INTEGER;

WITH ordered AS (
    SELECT id,
           ROW_NUMBER() OVER (PARTITION BY plan_id
                              ORDER BY created_at, id) AS rn
    FROM tasks
)
UPDATE tasks
   SET local_seq = (SELECT rn FROM ordered WHERE ordered.id = tasks.id)
 WHERE local_seq IS NULL;

CREATE TRIGGER IF NOT EXISTS tasks_fill_local_seq
AFTER INSERT ON tasks
WHEN new.local_seq IS NULL
BEGIN
    UPDATE tasks
       SET local_seq = COALESCE(
           (SELECT MAX(local_seq) + 1 FROM tasks
             WHERE plan_id = new.plan_id AND id <> new.id),
           1)
     WHERE id = new.id;
END;

CREATE UNIQUE INDEX IF NOT EXISTS idx_tasks_plan_local_seq
    ON tasks(plan_id, local_seq);

-- ===== source: 0017_plans_code_autogen.sql =====
-- Migration 0017: every plan carries a user-facing code.
--
-- `plans.code` has always been optional. In practice, the absence of a
-- code forces phase and task refs into an ugly `plan_<id>/...` fallback
-- and makes handoffs ambiguous. Backfill every null code as `P<id>`, and
-- keep the invariant going forward with a trigger so every insert either
-- uses the user-supplied code or auto-generates one. The column remains
-- nullable at the schema level because SQLite cannot add NOT NULL to an
-- existing column without a table rebuild; the trigger is authoritative.

UPDATE plans SET code = 'P' || id WHERE code IS NULL;

CREATE TRIGGER IF NOT EXISTS plans_autogen_code
AFTER INSERT ON plans
WHEN new.code IS NULL
BEGIN
    UPDATE plans SET code = 'P' || id WHERE id = new.id;
END;

-- ===== source: 0018_semantic_labels.sql =====
-- Migration 0018: semantic labels

-- Plans
ALTER TABLE plans ADD COLUMN uid TEXT;
ALTER TABLE plans ADD COLUMN label TEXT;

UPDATE plans SET uid = 'plan_' || id WHERE uid IS NULL;
UPDATE plans SET label = 'plan-' || id WHERE label IS NULL;

CREATE UNIQUE INDEX IF NOT EXISTS idx_plans_uid ON plans(uid);
CREATE UNIQUE INDEX IF NOT EXISTS idx_plans_label ON plans(label);

-- Phases
ALTER TABLE phases ADD COLUMN uid TEXT;
ALTER TABLE phases ADD COLUMN label TEXT;

UPDATE phases SET uid = 'phase_' || id WHERE uid IS NULL;
UPDATE phases SET label = 'phase-' || id WHERE label IS NULL;

CREATE UNIQUE INDEX IF NOT EXISTS idx_phases_uid ON phases(uid);
CREATE UNIQUE INDEX IF NOT EXISTS idx_phases_plan_label ON phases(plan_id, label);

-- Tasks
ALTER TABLE tasks ADD COLUMN uid TEXT;
ALTER TABLE tasks ADD COLUMN label TEXT;

UPDATE tasks SET uid = 'task_' || id WHERE uid IS NULL;
UPDATE tasks SET label = 'task-' || id WHERE label IS NULL;

CREATE UNIQUE INDEX IF NOT EXISTS idx_tasks_uid ON tasks(uid);
CREATE UNIQUE INDEX IF NOT EXISTS idx_tasks_plan_label ON tasks(plan_id, label);

-- ===== source: 0019_instructions.sql =====
-- Migration 0019: first-class instructions

CREATE TABLE IF NOT EXISTS instructions (
    id INTEGER PRIMARY KEY,
    entity_type TEXT NOT NULL CHECK (entity_type IN ('plan','phase','task')),
    entity_id INTEGER NOT NULL CHECK (entity_id > 0),
    instruction_type TEXT NOT NULL DEFAULT 'guidance' CHECK (length(trim(instruction_type)) > 0),
    body TEXT NOT NULL CHECK (length(trim(body)) > 0),
    author TEXT NOT NULL CHECK (length(trim(author)) > 0),
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at TEXT,
    invalidated_at TEXT,
    invalidated_by TEXT
);

CREATE INDEX IF NOT EXISTS idx_instructions_entity
    ON instructions(entity_type, entity_id, created_at);

CREATE TABLE IF NOT EXISTS events_new (
    id INTEGER PRIMARY KEY,
    entity_type TEXT NOT NULL CHECK (entity_type IN ('plan','phase','task')),
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
        'instruction_updated','instruction_invalidated'
    )),
    actor TEXT NOT NULL CHECK (length(trim(actor)) > 0),
    event_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    summary TEXT NOT NULL,
    details TEXT,
    old_value TEXT,
    new_value TEXT,
    related_entity_type TEXT CHECK (related_entity_type IS NULL OR related_entity_type IN ('plan','phase','task')),
    related_entity_id INTEGER CHECK (related_entity_id IS NULL OR related_entity_id > 0),
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

CREATE INDEX IF NOT EXISTS idx_events_entity ON events(entity_type, entity_id, event_at);
CREATE INDEX IF NOT EXISTS idx_events_event_at ON events(event_at);
CREATE INDEX IF NOT EXISTS idx_events_type_at ON events(event_type, event_at);
