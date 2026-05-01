-- Migration 0002: extend closure_records with git capture fields
--
-- Adds optional fields populated by the CLI auto-capture path on task.close
-- (Phase 11 / ipman v2.1). The op handler does not invoke git; these columns
-- accept whatever the CLI (or any other client) chooses to send. All three
-- columns are NULL on rows written before this migration and on closures
-- recorded outside a git context.

ALTER TABLE closure_records
    ADD COLUMN commit_sha TEXT
        CHECK (commit_sha IS NULL OR length(trim(commit_sha)) > 0);

ALTER TABLE closure_records
    ADD COLUMN dirty INTEGER
        CHECK (dirty IS NULL OR dirty IN (0, 1));

ALTER TABLE closure_records
    ADD COLUMN files_changed_json TEXT;
