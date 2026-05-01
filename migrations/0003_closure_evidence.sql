-- Migration 0003: extend closure_records with structured evidence fields
--
-- Adds optional structured-evidence columns populated by task.close (Phase 12
-- / ipman v2.1). The op handler does not interpret these JSON blobs as more
-- than text on write; consumers (closure.get, plan markdown render) parse
-- them as arrays of objects. Both columns are NULL when the client does not
-- send any evidence.

ALTER TABLE closure_records
    ADD COLUMN validations_json TEXT;

ALTER TABLE closure_records
    ADD COLUMN decisions_json TEXT;
