#include "agent_docs.h"

#include "log.h"
#include "ipman_home.h"

#include <cJSON.h>

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define IPMAN_AGENT_DOCS_GENERATOR_VERSION "1.0.0"
#define IPMAN_AGENT_DOCS_CONTENT_REVISION "2026-04-29.1"

typedef struct {
    const char *name;
    const char *entity;
    const char *intent;
    const char *summary;
    const char *required;
    const char *optional;
    const char *validations;
    const char *preconditions;
    const char *side_effects;
    const char *errors;
    const char *response;
    const char *related;
    const char *example_params;
    const char *schema_required;
    const char *output_fields;
} OperationSpec;

typedef struct {
    const char *name;
    const char *purpose;
    const char *fields;
    const char *invariants;
    const char *operations;
    const char *guidance;
} EntitySpec;

typedef struct {
    char *rel_path;
    char *content;
    char hash_hex[17];
} GeneratedArtifact;

typedef struct {
    GeneratedArtifact *items;
    size_t len;
    size_t cap;
} ArtifactList;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buf;

static int op_uses_phase_selector(const char *name);
static int op_uses_phase_or_phase_id_selector(const char *name);
static int op_uses_task_selector(const char *name);
static int op_uses_plan_selector(const char *name);

static const OperationSpec k_operation_specs[] = {
    { "noop", "workspace", "inspect", "Return an empty success result; useful for smoke tests.", "none", "none", "params must be an object, normally empty.", "Initialized workspace.", "Runs normal startup checks and returns no business data.", "internal_error if response allocation fails.", "result is an empty object.", "workspace.refresh_agent_docs", "{}", "", "" },
    { "closure.get", "closure_record", "handoff", "Read closure memory for a plan, phase, or task.", "entity_type, entity_id", "none", "entity_type must be plan, phase, or task; entity_id must be positive.", "The referenced entity must exist.", "No business data changes.", "validation_failed, not_found, internal_error.", "result.active_closure and result.closures.", "task.close, task.cancel, task.replace, phase.close", "{\"entity_type\":\"task\",\"entity_id\":1}", "\"entity_type\",\"entity_id\"", "active_closure, closures" },
    { "comment.add", "comment", "ongoing execution", "Add a comment to any plan, phase, or task.", "entity_type, entity_id, body", "comment_type", "body and comment_type must be non-empty strings when provided.", "The referenced entity must exist.", "Creates a comment and emits comment_added.", "validation_failed, not_found, internal_error.", "result.comment.", "comment.list, comment.update, comment.invalidate", "{\"entity_type\":\"task\",\"entity_id\":1,\"body\":\"Observed risk while implementing.\",\"comment_type\":\"progress\"}", "\"entity_type\",\"entity_id\",\"body\"", "comment" },
    { "comment.invalidate", "comment", "ongoing execution", "Soft-invalidate a comment while preserving audit history.", "id", "none", "id must be a positive integer.", "The comment must exist and not already be invalidated.", "Sets invalidated fields and emits comment_invalidated.", "validation_failed, not_found, conflict, internal_error.", "result.comment.", "comment.list, comment.update", "{\"id\":1}", "\"id\"", "comment" },
    { "comment.list", "comment", "inspect", "List comments attached to an entity.", "entity_type, entity_id", "include_invalidated", "entity_type must be plan, phase, or task; include_invalidated must be boolean.", "The referenced entity must exist.", "No business data changes.", "validation_failed, not_found, internal_error.", "result.comments, result.include_invalidated, result.has_more, result.total_count.", "comment.add, comment.update, comment.invalidate", "{\"entity_type\":\"task\",\"entity_id\":1,\"include_invalidated\":false}", "\"entity_type\",\"entity_id\"", "comments, include_invalidated, has_more, total_count" },
    { "comment.update", "comment", "ongoing execution", "Edit a recent non-invalidated comment.", "id, body", "none", "id must be positive; body must be non-empty.", "The comment must exist, be valid, and be inside the edit window.", "Updates body and emits comment_updated.", "validation_failed, not_found, conflict, internal_error.", "result.comment.", "comment.list, comment.invalidate", "{\"id\":1,\"body\":\"Updated progress note.\"}", "\"id\",\"body\"", "comment" },
    { "event.list", "event", "audit/history", "List audit events globally or for one entity.", "none", "entity_type with entity_id, event_type, from, to, request_id, limit, offset", "entity_type/entity_id must be provided together; limit is 1..500; offset is >=0; event_type must be known; request_id is matched exactly when supplied.", "If scoped, the entity type must be valid.", "No business data changes.", "validation_failed, internal_error.", "result.events, result.limit, result.offset, result.has_more, result.total_count.", "plan.history, phase.history", "{\"entity_type\":\"task\",\"entity_id\":1,\"limit\":50}", "", "events, limit, offset, has_more, total_count" },
    { "instruction.add", "instruction", "ongoing execution", "Add durable guidance to a plan, phase, or task.", "entity_type, entity_id, body", "instruction_type", "entity_type must be plan, phase, or task; entity_id must be positive; body and instruction_type must be non-empty strings when provided.", "The referenced entity must exist.", "Creates an instruction and emits instruction_added on the referenced entity.", "validation_failed, not_found, internal_error.", "result.instruction.", "instruction.list, instruction.update, instruction.invalidate", "{\"entity_type\":\"phase\",\"entity_id\":1,\"body\":\"Keep this phase focused on API compatibility.\",\"instruction_type\":\"guidance\"}", "\"entity_type\",\"entity_id\",\"body\"", "instruction" },
    { "instruction.invalidate", "instruction", "ongoing execution", "Deactivate an instruction while preserving audit history.", "id", "none", "id must be a positive integer.", "The instruction must exist and not already be invalidated.", "Sets invalidated fields and emits instruction_invalidated on the referenced entity.", "validation_failed, not_found, conflict, internal_error.", "result.instruction.", "instruction.list, instruction.update", "{\"id\":1}", "\"id\"", "instruction" },
    { "instruction.list", "instruction", "inspect", "List active guidance attached to a plan, phase, or task.", "entity_type, entity_id", "include_invalidated", "entity_type must be plan, phase, or task; include_invalidated must be boolean.", "The referenced entity must exist.", "No business data changes.", "validation_failed, not_found, internal_error.", "result.instructions, result.include_invalidated, result.has_more, result.total_count.", "instruction.add, instruction.update, instruction.invalidate", "{\"entity_type\":\"plan\",\"entity_id\":1}", "\"entity_type\",\"entity_id\"", "instructions, include_invalidated, has_more, total_count" },
    { "instruction.update", "instruction", "ongoing execution", "Update the text, and optionally the type, of an active instruction.", "id, body", "instruction_type", "id must be positive; body must be non-empty; instruction_type must be non-empty when provided.", "The instruction must exist and not be invalidated.", "Updates instruction fields and emits instruction_updated on the referenced entity.", "validation_failed, not_found, conflict, internal_error.", "result.instruction.", "instruction.list, instruction.invalidate", "{\"id\":1,\"body\":\"Keep compatibility, but prefer simpler adapters.\",\"instruction_type\":\"guidance\"}", "\"id\",\"body\"", "instruction" },
    { "phase.close", "phase", "close", "Close a phase with closure memory.", "one of id, uid, or label; outcome_summary; closing_comment", "plan_uid or plan_label (scope required when using label); lessons_learned, open_items_summary, followup_needed, outcome", "outcome is completed or canceled; closure text must be non-empty; followup_needed is boolean; label requires plan_uid or plan_label scope.", "Phase must exist and all tasks in it must be terminal.", "Updates phase, emits phase_closed, writes closure record.", "validation_failed, not_found, conflict, internal_error.", "result.phase.", "phase.reopen, closure.get, phase.history", "{\"id\":1,\"outcome_summary\":\"Phase objectives met.\",\"closing_comment\":\"All planned work is terminal.\",\"outcome\":\"completed\"}", "\"outcome_summary\",\"closing_comment\"", "phase" },
    { "phase.clear_current", "workspace_context", "ongoing execution", "Clear the current phase for the active plan.", "none", "none", "params must be an object, normally empty.", "An active plan must be set.", "Clears plan_contexts.current_phase_id, clears any phased current task, and emits phase_current_changed when changed.", "conflict, internal_error.", "result.context.", "workspace.context_get, phase.set_current", "{}", "", "context" },
    { "phase.comment_add", "comment", "ongoing execution", "Add a comment to a phase.", "one of id, uid, or label; body", "comment_type; plan_uid or plan_label (scope required when using label)", "body must be non-empty; label requires plan_uid or plan_label scope.", "The phase must exist.", "Creates a comment and emits comment_added.", "validation_failed, not_found, internal_error.", "result.comment.", "comment.list, phase.history", "{\"id\":1,\"body\":\"Phase implementation is underway.\",\"comment_type\":\"progress\"}", "\"body\"", "comment" },
    { "phase.create", "phase", "create/bootstrap", "Create an ordered phase inside a plan.", "plan_id, title", "summary, description, status, sequence_no, owner, target_start_date, target_end_date, label", "title must be non-empty; status, if provided, is open; sequence_no must be positive; label is auto-generated from title if omitted.", "Plan must exist.", "Creates phase with auto-generated uid and label, emits phase_created.", "validation_failed, not_found, conflict, internal_error.", "result.phase.", "plan.create, phase.move, task.create", "{\"plan_id\":1,\"title\":\"Implementation\",\"summary\":\"Build the first working increment\"}", "\"plan_id\",\"title\"", "phase" },
    { "phase.get", "phase", "inspect", "Fetch one phase.", "one of id, uid, or label+scope", "plan_uid or plan_label (scope required when using label)", "When using label, plan_uid or plan_label must also be supplied.", "Phase must exist.", "No business data changes.", "validation_failed, not_found, internal_error.", "result.phase.", "phase.list, phase.history", "{\"id\":1}", "", "phase" },
    { "phase.history", "phase", "audit/history", "Read audit events for one phase.", "one of id, uid, or label", "since; plan_uid or plan_label (scope required when using label)", "since must be a positive integer when provided; label requires plan_uid or plan_label scope.", "Phase must exist.", "No business data changes.", "validation_failed, not_found, internal_error.", "result.events.", "event.list, phase.get", "{\"id\":1,\"since\":1}", "", "events" },
    { "phase.list", "phase", "inspect", "List phases, optionally filtered.", "none", "plan_id, status, completed_or_blocked, limit, offset", "status must be a phase status; completed_or_blocked is boolean; limit is 1..500.", "none.", "No business data changes.", "validation_failed, internal_error.", "result.phases, result.limit, result.offset, result.has_more, result.total_count.", "phase.get, phase.list_tasks", "{\"plan_id\":1,\"limit\":100}", "", "phases, limit, offset, has_more, total_count" },
    { "phase.list_tasks", "task", "inspect", "List tasks in a phase.", "one of id, uid, label, or phase_id", "status; plan_uid or plan_label (scope required when using label)", "id and phase_id must be positive; phase_id is an alias that must match any resolved id, uid, or label selector supplied with it; status must be a task status; label requires plan_uid or plan_label scope.", "Phase must exist.", "No business data changes.", "validation_failed, not_found, internal_error.", "result.tasks.", "task.list, phase.get", "{\"id\":1,\"status\":\"todo\"}", "", "tasks" },
    { "phase.move", "phase", "ongoing execution", "Move a phase to another sequence number.", "one of id, uid, or label; sequence_no", "plan_uid or plan_label (scope required when using label)", "sequence_no must be a positive integer; label requires plan_uid or plan_label scope.", "Phase must exist.", "Reorders phases and emits phase_moved when changed.", "validation_failed, not_found, conflict, internal_error.", "result.phase.", "phase.list", "{\"id\":1,\"sequence_no\":2}", "\"sequence_no\"", "phase" },
    { "phase.progress", "phase", "inspect", "Summarize task status counts for a phase.", "one of id, uid, or label", "plan_uid or plan_label (scope required when using label)", "label requires plan_uid or plan_label scope.", "Phase must exist.", "No business data changes.", "validation_failed, not_found, internal_error.", "status counts, total, completion_percentage.", "phase.list_tasks, task.list", "{\"id\":1}", "", "phase_id, plan_id, counts, total, completion_percentage" },
    { "phase.reopen", "phase", "ongoing execution", "Reopen a completed or canceled phase.", "one of id, uid, or label", "plan_uid or plan_label (scope required when using label)", "label requires plan_uid or plan_label scope.", "Phase must exist and be completed or canceled.", "Sets phase to open and emits phase_reopened.", "validation_failed, not_found, conflict, internal_error.", "result.phase.", "phase.close", "{\"id\":1}", "", "phase" },
    { "phase.set_current", "workspace_context", "ongoing execution", "Set the current phase for the active plan.", "id", "none", "id must be positive.", "An active plan must be set; phase must exist, belong to it, and not be terminal.", "Updates plan_contexts.current_phase_id, may clear incompatible current_task_id, and emits phase_current_changed when changed.", "validation_failed, not_found, conflict, internal_error.", "result.context.", "workspace.context_get, task.set_current", "{\"id\":1}", "\"id\"", "context" },
    { "phase.update", "phase", "ongoing execution", "Update editable phase fields.", "one of id, uid, or label; plus at least one updated field", "plan_uid or plan_label (scope required when using label); title, summary, description, owner, target_start_date, target_end_date", "title must remain non-empty; nullable fields may be null; label requires plan_uid or plan_label scope.", "Phase must exist.", "Updates phase and emits phase_updated.", "validation_failed, not_found, conflict, internal_error.", "result.phase.", "phase.get, phase.history", "{\"id\":1,\"summary\":\"Updated phase scope\"}", "", "phase" },
    { "plan.activate", "workspace_context", "ongoing execution", "Set the active plan for this workspace and restore its saved cursor.", "exactly one of id or code", "none", "selector must be valid.", "Plan must exist and not be completed, canceled, or archived.", "Updates workspace_context, creates a plan_contexts row if needed, and emits plan_activated when changed.", "validation_failed, not_found, conflict, internal_error.", "result.context.", "workspace.context_get, plan.deactivate", "{\"code\":\"REL-001\"}", "", "context" },
    { "plan.archive", "plan", "close", "Archive a completed or canceled plan.", "one of id, uid, code, or label", "none", "id must be positive; code, uid, and label must be non-empty strings.", "Plan must exist and be completed or canceled.", "Sets status archived and emits plan_archived.", "validation_failed, not_found, conflict, internal_error.", "result.plan.", "plan.close, plan.reopen", "{\"code\":\"REL-001\"}", "", "plan" },
    { "plan.close", "plan", "close", "Close a plan as completed or canceled.", "one of id, uid, code, or label", "outcome, outcome_summary, closing_comment, lessons_learned, open_items_summary, followup_needed", "outcome is completed or canceled and defaults to completed; id must be positive; code, uid, and label must be non-empty strings.", "Plan must exist and be open, in_progress, or paused.", "Updates plan status and emits plan_closed.", "validation_failed, not_found, conflict, internal_error.", "result.plan.", "plan.reopen, plan.archive, plan.history", "{\"code\":\"REL-001\",\"outcome\":\"completed\"}", "", "plan" },
    { "plan.comment_add", "comment", "ongoing execution", "Add a comment to a plan.", "id, body", "comment_type", "id must be positive; body must be non-empty.", "Plan must exist.", "Creates a comment and emits comment_added.", "validation_failed, not_found, internal_error.", "result.comment.", "comment.list, plan.history", "{\"id\":1,\"body\":\"Release scope approved.\",\"comment_type\":\"decision\"}", "\"id\",\"body\"", "comment" },
    { "plan.create", "plan", "create/bootstrap", "Create the top-level plan container.", "title", "code, summary, description, status, priority, owner, target_date, tags, version_label, label", "title must be non-empty; status and priority must use public enums; tags is an array of strings or null; label is auto-generated from title if omitted.", "none.", "Creates plan with auto-generated uid and label, emits plan_created.", "validation_failed, conflict, internal_error.", "result.plan.", "phase.create, task.create, plan.list", "{\"code\":\"REL-001\",\"title\":\"Release 1\",\"summary\":\"Prepare release\",\"priority\":\"high\",\"tags\":[\"release\"]}", "\"title\"", "plan" },
    { "plan.deactivate", "workspace_context", "ongoing execution", "Clear the active plan for this workspace without deleting per-plan cursors.", "none", "none", "params must be an object, normally empty.", "none.", "Clears workspace_context.active_plan_id and emits plan_deactivated when a plan was active.", "internal_error.", "result.context.", "workspace.context_get, plan.activate", "{}", "", "context" },
    { "plan.export", "plan", "audit/history", "Export a plan and its dependents (phases, tasks, instructions, comments, events, closures, relations) as a canonical JSON snapshot.", "plan_id", "none", "plan_id must be a positive integer.", "Plan must exist.", "No business data changes; emits no events.", "validation_failed, not_found, internal_error.", "result.export.", "plan.get, plan.history", "{\"plan_id\":1}", "\"plan_id\"", "export" },
    { "plan.get", "plan", "inspect", "Fetch one plan by id, code, uid, or label.", "one of id, code, uid, or label", "none", "id must be positive; code, uid, and label must be non-empty strings.", "Plan must exist.", "No business data changes.", "validation_failed, not_found, internal_error.", "result.plan.", "plan.list, plan.history", "{\"code\":\"REL-001\"}", "", "plan" },
    { "plan.history", "plan", "audit/history", "Read audit events for one plan.", "one of id, uid, code, or label", "since", "id must be positive; code, uid, and label must be non-empty; since is a positive event id.", "Plan must exist.", "No business data changes.", "validation_failed, not_found, internal_error.", "result.events.", "event.list, plan.get", "{\"code\":\"REL-001\",\"since\":1}", "", "events" },
    { "plan.list", "plan", "inspect", "List plans with optional filters.", "none", "status, owner, tag, limit, offset", "filters must be strings or null; status must be a plan status; limit is 1..500.", "none.", "No business data changes.", "validation_failed, internal_error.", "result.plans, result.limit, result.offset, result.has_more, result.total_count.", "plan.get, plan.create", "{\"status\":\"open\"}", "", "plans, limit, offset, has_more, total_count" },
    { "plan.progress", "plan", "inspect", "Summarize task status counts for a plan.", "one of id, uid, code, or label", "none", "id must be positive; code, uid, and label must be non-empty strings.", "Plan must exist.", "No business data changes.", "validation_failed, not_found, internal_error.", "status counts, total, completion_percentage.", "task.list, phase.progress", "{\"code\":\"REL-001\"}", "", "plan_id, counts, total, completion_percentage" },
    { "plan.reopen", "plan", "ongoing execution", "Reopen a completed, canceled, or archived plan.", "one of id, uid, code, or label", "none", "id must be positive; code, uid, and label must be non-empty strings.", "Plan must exist and be completed, canceled, or archived.", "Sets status open and emits plan_reopened.", "validation_failed, not_found, conflict, internal_error.", "result.plan.", "plan.close, plan.archive", "{\"code\":\"REL-001\"}", "", "plan" },
    { "plan.update", "plan", "ongoing execution", "Update editable plan fields.", "one of id, uid, code, or label; plus at least one updated field", "title, summary, description, priority, owner, target_date, tags, version_label", "title must remain non-empty; priority and tags are validated; nullable fields may be null; id must be positive; code, uid, and label must be non-empty strings.", "Plan must exist.", "Updates plan and emits plan_updated.", "validation_failed, not_found, conflict, internal_error.", "result.plan.", "plan.get, plan.history", "{\"code\":\"REL-001\",\"summary\":\"Updated release scope\",\"priority\":\"critical\"}", "", "plan" },
    { "task.assign", "task", "ongoing execution", "Assign a task.", "one of id, uid, or label; assignee", "plan_uid or plan_label (scope required when using label)", "assignee must be non-empty; label requires plan_uid or plan_label scope.", "Task must exist.", "Updates task and emits task_updated.", "validation_failed, not_found, internal_error.", "result.task.", "task.unassign, task.get", "{\"id\":1,\"assignee\":\"alex\"}", "\"assignee\"", "task" },
    { "task.cancel", "task", "cancel", "Cancel a task with terminal resolution and closure memory.", "one of id, uid, or label; resolution; closing_comment or comment", "plan_uid or plan_label (scope required when using label); outcome_summary, reason_code, reason_text, lessons_learned, open_items_summary, followup_needed", "resolution must be canceled, not_planned, discarded, or duplicate; closure memory must be non-empty; label requires plan_uid or plan_label scope.", "Task must exist and be eligible to move to canceled.", "Sets status canceled, writes closure record, may write closure comment, emits task_canceled.", "validation_failed, not_found, conflict, internal_error.", "result.task.", "task.mark_duplicate, task.replace, closure.get", "{\"id\":1,\"resolution\":\"not_planned\",\"closing_comment\":\"No longer in scope.\",\"outcome_summary\":\"Canceled before implementation.\"}", "\"resolution\"", "task" },
    { "task.close", "task", "close", "Close a task as completed with closure memory.", "one of id, uid, or label; outcome_summary; closing_comment", "plan_uid or plan_label (scope required when using label); lessons_learned, open_items_summary, followup_needed", "closure text must be non-empty; followup_needed is boolean; label requires plan_uid or plan_label scope.", "Task must exist, be eligible to move to done, and not already have a resolution.", "Sets status done, resolution completed, writes closure record, emits task_closed.", "validation_failed, not_found, conflict, internal_error.", "result.task.", "closure.get, task.reopen", "{\"id\":1,\"outcome_summary\":\"Implemented and tested.\",\"closing_comment\":\"Ready for review.\"}", "\"outcome_summary\",\"closing_comment\"", "task" },
    { "task.comment_add", "comment", "ongoing execution", "Add a comment to a task.", "one of id, uid, or label; body", "comment_type; plan_uid or plan_label (scope required when using label)", "body must be non-empty; label requires plan_uid or plan_label scope.", "Task must exist.", "Creates a comment and emits comment_added.", "validation_failed, not_found, internal_error.", "result.comment.", "comment.list, task.history via event.list", "{\"id\":1,\"body\":\"Starting implementation now.\",\"comment_type\":\"progress\"}", "\"body\"", "comment" },
    { "task.create", "task", "create/bootstrap", "Create a task in a plan.", "plan_id, title", "phase_id, parent_task_id, parent_uid, parent_label, summary, description, status, priority, task_type, origin_type, assignee, due_date, target_start_date, estimate, origin_ref_type, origin_ref_id, origin_task_id, label", "title must be non-empty; status can only be todo on create; priority, task_type, and origin_type use public enums; label is auto-generated from title if omitted; parent_task_id, parent_uid, and parent_label are mutually exclusive.", "Plan must exist; phase, parent, and origin task must belong to the same plan when provided.", "Creates task with auto-generated uid and label, emits task_created.", "validation_failed, not_found, conflict, internal_error.", "result.task.", "task.transition, task.defer, task.close", "{\"plan_id\":1,\"phase_id\":1,\"title\":\"Implement feature\",\"priority\":\"high\",\"task_type\":\"task\",\"origin_type\":\"planned\"}", "\"plan_id\",\"title\"", "task" },
    { "task.clear_current", "workspace_context", "ongoing execution", "Clear the current task for the active plan.", "none", "none", "params must be an object, normally empty.", "An active plan must be set.", "Clears plan_contexts.current_task_id and emits task_current_changed when changed.", "conflict, internal_error.", "result.context.", "workspace.context_get, task.set_current", "{}", "", "context" },
    { "task.defer", "task", "defer", "Defer active work while preserving why it moved out of current scope.", "one of id, uid, or label", "plan_uid or plan_label (scope required when using label); deferred_until, reason_code, reason_text", "At least one of reason_code or reason_text is required; label requires plan_uid or plan_label scope.", "Task must exist and be todo, in_progress, blocked, or already deferred.", "Sets status deferred, stores deferral fields, emits task_deferred.", "validation_failed, not_found, conflict, internal_error.", "result.task.", "task.transition, task.cancel, task.replace", "{\"id\":1,\"deferred_until\":\"2026-05-01\",\"reason_code\":\"waiting-on-input\",\"reason_text\":\"Need product decision before continuing.\"}", "", "task" },
    { "task.get", "task", "inspect", "Fetch one task.", "one of id, uid, or label+scope", "plan_uid or plan_label (scope required when using label)", "When using label, plan_uid or plan_label must also be supplied.", "Task must exist.", "No business data changes.", "validation_failed, not_found, internal_error.", "result.task.", "task.list, comment.list, closure.get", "{\"id\":1}", "", "task" },
    { "task.link_dependency", "task_relation", "ongoing execution", "Create a task relation such as blocks or related.", "id, target_task_id, relation_type", "notes", "relation_type is blocks, blocked_by, related, or duplicates; endpoints must differ.", "Both tasks must exist in the same plan; blocks relations must not create cycles.", "Creates relation and sometimes reverse relation; emits task_updated.", "validation_failed, not_found, conflict, internal_error.", "result.relation and result.reverse_relation.", "task.unlink_dependency, task.list", "{\"id\":1,\"target_task_id\":2,\"relation_type\":\"blocks\",\"notes\":\"Task 2 waits for task 1.\"}", "\"target_task_id\",\"relation_type\"", "relation, reverse_relation" },
    { "task.link_external", "task", "ongoing execution", "Link a task to an external artifact using origin_ref_type and origin_ref_id.", "task_id, ref_type, ref_id", "none", "ref_type must be github_issue, gitlab_issue, commit, pr, adr, url, or file; task_id must be positive; ref_id must be non-empty.", "Task must exist.", "Updates external reference fields and emits task_linked_external.", "validation_failed, not_found, internal_error.", "result.task.", "task.set_origin, task.get", "{\"task_id\":1,\"ref_type\":\"github_issue\",\"ref_id\":\"123\"}", "\"task_id\",\"ref_type\",\"ref_id\"", "task" },
    { "task.list", "task", "inspect", "List tasks using operational filters.", "none", "plan_id, phase_id, status, origin_type, resolution, assignee, priority, blocked, deferred, pending, added_after_original, canceled, replaced, blocked_by_other, limit, offset", "enum filters must be valid; booleans must be true or false; limit is 1..500; offset is >=0.", "none.", "No business data changes.", "validation_failed, internal_error.", "result.tasks, result.limit, result.offset, result.has_more, result.total_count.", "task.get, plan.progress, phase.list_tasks", "{\"plan_id\":1,\"pending\":true,\"limit\":100}", "", "tasks, limit, offset, has_more, total_count" },
    { "task.mark_duplicate", "task", "cancel", "Cancel one task as a duplicate of another.", "one of id, uid, or label; target_task_id; closing_comment or comment", "plan_uid or plan_label (scope required when using label); outcome_summary, reason_code, reason_text, relation_notes, lessons_learned, open_items_summary, followup_needed", "endpoints must differ; closure memory must be non-empty; label requires plan_uid or plan_label scope.", "Both tasks must exist in the same plan; source task must be cancelable.", "Sets source status canceled/resolution duplicate, creates duplicates relation, writes closure record.", "validation_failed, not_found, conflict, internal_error.", "result.task, result.relation, result.comment_id.", "task.cancel, task.link_dependency", "{\"id\":1,\"target_task_id\":2,\"closing_comment\":\"Duplicate of task 2.\",\"outcome_summary\":\"Merged into existing task.\"}", "\"target_task_id\"", "task, relation, comment_id" },
    { "task.move", "task", "ongoing execution", "Move a task to a phase or clear its phase.", "id, phase_id", "none", "id must be positive; phase_id must be positive or null.", "Task must exist; target phase must exist in same plan if provided.", "Updates task phase and emits task_updated.", "validation_failed, not_found, conflict, internal_error.", "result.task.", "phase.list_tasks, task.get", "{\"id\":1,\"phase_id\":2}", "\"phase_id\"", "task" },
    { "task.reopen", "task", "ongoing execution", "Reopen a terminal task.", "one of id, uid, or label", "plan_uid or plan_label (scope required when using label)", "label requires plan_uid or plan_label scope.", "Task must exist, be terminal, not resolution=replaced, and if it belongs to a phase that phase must not be terminal (reopen the phase first).", "Sets task to todo, clears terminal fields, emits task_reopened.", "validation_failed, not_found, conflict, internal_error.", "result.task.", "task.close, task.cancel", "{\"id\":1}", "", "task" },
    { "task.replace", "task", "replace", "Cancel an old task as replaced and create the replacement task.", "one of id, uid, or label; title; closing_comment or comment", "plan_uid or plan_label (scope required when using label); phase_id, parent_task_id, summary, description, priority, task_type, assignee, due_date, target_start_date, estimate, reason_code, reason_text, relation_notes, outcome_summary, lessons_learned, open_items_summary, followup_needed", "replacement title must be non-empty; closure memory must be non-empty; priority and task_type are enums; label requires plan_uid or plan_label scope.", "Old task must be replaceable; replacement phase/parent must be in same plan.", "Cancels old task with resolution replaced, creates new task with origin_type replacement, creates replaces relation, writes closure record.", "validation_failed, not_found, conflict, internal_error.", "result.old_task, result.new_task, result.relation_id.", "task.cancel, task.close, closure.get", "{\"id\":1,\"title\":\"Implement revised feature\",\"closing_comment\":\"Original scope changed.\",\"outcome_summary\":\"Replaced with narrower task.\",\"priority\":\"high\"}", "\"title\"", "old_task, new_task, relation_id" },
    { "task.set_origin", "task", "ongoing execution", "Set origin metadata for a task.", "one of id, uid, or label; origin_type", "plan_uid or plan_label (scope required when using label); origin_ref_type, origin_ref_id, origin_task_id", "origin_type must be planned, addendum, discovered, replacement, carryover, or external_request; label requires plan_uid or plan_label scope.", "Task must exist; origin_task_id must exist in same plan when provided.", "Updates origin fields and emits task_updated.", "validation_failed, not_found, conflict, internal_error.", "result.task.", "task.get, task.list", "{\"id\":1,\"origin_type\":\"discovered\",\"origin_ref_type\":\"review\",\"origin_ref_id\":\"PR-12\"}", "\"origin_type\"", "task" },
    { "task.set_priority", "task", "ongoing execution", "Set task priority.", "one of id, uid, or label; priority", "plan_uid or plan_label (scope required when using label)", "priority must be low, medium, high, or critical; label requires plan_uid or plan_label scope.", "Task must exist.", "Updates priority and emits task_updated.", "validation_failed, not_found, internal_error.", "result.task.", "task.update, task.list", "{\"id\":1,\"priority\":\"critical\"}", "\"priority\"", "task" },
    { "task.set_current", "workspace_context", "ongoing execution", "Set the current task for the active plan.", "id", "none", "id must be positive.", "An active plan must be set; task must exist, belong to it, and not be terminal. If the task has a phase, that phase must not be terminal.", "Updates plan_contexts.current_task_id, syncs current_phase_id to the task phase when present, and emits task_current_changed when changed.", "validation_failed, not_found, conflict, internal_error.", "result.context.", "workspace.context_get, phase.set_current", "{\"id\":1}", "\"id\"", "context" },
    { "task.set_type", "task", "ongoing execution", "Set task type.", "one of id, uid, or label; task_type", "plan_uid or plan_label (scope required when using label)", "task_type must be task, research, bug, decision, review, or documentation; label requires plan_uid or plan_label scope.", "Task must exist.", "Updates task_type and emits task_updated.", "validation_failed, not_found, internal_error.", "result.task.", "task.update, task.list", "{\"id\":1,\"task_type\":\"bug\"}", "\"task_type\"", "task" },
    { "task.transition", "task", "ongoing execution", "Move a task through non-terminal generic statuses.", "one of id, uid, or label; status", "plan_uid or plan_label (scope required when using label)", "status must be a valid task status; dedicated operations are required for deferred, done, and canceled terminal flows; label requires plan_uid or plan_label scope.", "Task must exist and transition must be allowed.", "Updates status and emits task_status_changed.", "validation_failed, not_found, conflict, internal_error.", "result.task.", "task.defer, task.close, task.cancel", "{\"id\":1,\"status\":\"in_progress\"}", "\"status\"", "task" },
    { "task.unassign", "task", "ongoing execution", "Clear a task assignee.", "one of id, uid, or label", "plan_uid or plan_label (scope required when using label)", "label requires plan_uid or plan_label scope.", "Task must exist.", "Clears assignee and emits task_updated.", "validation_failed, not_found, internal_error.", "result.task.", "task.assign", "{\"id\":1}", "", "task" },
    { "task.unlink_dependency", "task_relation", "ongoing execution", "Remove a task relation.", "id, target_task_id, relation_type", "none", "relation_type is blocks, blocked_by, related, or duplicates; endpoints must differ.", "Both tasks and the relation must exist in the same plan.", "Deletes relation and reverse relation when applicable; emits task_updated.", "validation_failed, not_found, conflict, internal_error.", "result.relation, result.reverse_relation, result.deleted.", "task.link_dependency", "{\"id\":1,\"target_task_id\":2,\"relation_type\":\"blocks\"}", "\"target_task_id\",\"relation_type\"", "relation, reverse_relation, deleted" },
    { "task.update", "task", "ongoing execution", "Update editable task fields without changing semantic terminal state.", "one of id, uid, or label; plus at least one updated field", "plan_uid or plan_label (scope required when using label); title, summary, description, due_date, target_start_date, estimate, blocked_reason, reason_code, reason_text", "title must remain non-empty; nullable fields may be null; label requires plan_uid or plan_label scope.", "Task must exist.", "Updates task and emits task_updated.", "validation_failed, not_found, internal_error.", "result.task.", "task.set_priority, task.set_type, task.transition", "{\"id\":1,\"summary\":\"Updated implementation details\",\"estimate\":\"1d\"}", "", "task" },
    { "workspace.context_get", "workspace_context", "inspect", "Read the active plan and current phase/task cursor for this workspace.", "none", "none", "params must be an object, normally empty.", "Initialized workspace.", "No business data changes.", "internal_error.", "result.context.", "plan.activate, phase.set_current, task.set_current", "{}", "", "context" },
    { "workspace.refresh_agent_docs", "workspace", "create/bootstrap", "Regenerate the agent knowledge pack in the effective IPMAN_HOME.", "none", "none", "params must be an object, normally empty.", "Effective home must be initialized and writable.", "Refreshes generated workspace artifacts only; it does not modify business data.", "internal_error on filesystem or rendering failure.", "result.workspace_root, db_path, files_total, files_written, files_unchanged, fingerprints.", "noop", "{}", "", "workspace_root, db_path, files_total, files_written, files_unchanged, generated_at, source_fingerprint, content_fingerprint" },
};

static const EntitySpec k_entity_specs[] = {
    { "plan", "Top-level container for a body of work.", "id, uid (immutable system id e.g. plan_1), label (semantic slug unique globally), code, title, summary, description, status, priority, owner, target_date, tags, version_label, timestamps.", "Status is one of open, in_progress, paused, completed, canceled, archived. uid is stable for permanent references; label is the recommended daily selector. Plans are preserved and archived instead of being deleted.", "plan.create, plan.get, plan.list, plan.update, plan.close, plan.archive, plan.reopen, plan.history, plan.progress, plan.comment_add.", "Use uid or label in selectors instead of id whenever possible. Use plan operations for state changes instead of editing SQLite." },
    { "phase", "Ordered stage inside a plan.", "id, uid (immutable system id e.g. phase_1), label (semantic slug unique per plan), plan_id, title, summary, description, status, sequence_no, local_seq, owner, target_start_date, target_end_date, timestamps.", "A phase belongs to one plan and has a unique sequence number inside that plan. Closing a phase requires terminal tasks. uid is stable for permanent references; label (scoped by plan_uid or plan_label) is the recommended daily selector.", "phase.create, phase.get, phase.list, phase.update, phase.move, phase.close, phase.reopen, phase.history, phase.progress, phase.list_tasks, phase.comment_add.", "Use label with plan_uid or plan_label scope as the recommended selector." },
    { "task", "Primary operational unit of work.", "id, uid (immutable system id e.g. task_1), label (semantic slug unique per plan), plan_id, phase_id, parent_task_id, title, summary, description, status, resolution, priority, task_type, origin_type, assignee, deferred_until, blocked_reason, reason fields, due dates, origin metadata, local_seq, timestamps.", "status, resolution, and origin_type are separate. status is current workflow position; resolution explains terminal outcome; origin_type records why the task exists. uid is stable for permanent references; label (scoped by plan_uid or plan_label) is the recommended daily selector. Replacement links live in task_relations.", "task.create, task.get, task.list, task.update, task.transition, task.defer, task.cancel, task.replace, task.close, task.reopen, task.assign, task.unassign, task.set_priority, task.set_type, task.set_origin, task.link_external, task.link_dependency, task.unlink_dependency, task.mark_duplicate, task.comment_add.", "Use semantic operations for defer, cancel, replace, and close. Use label with plan_uid or plan_label scope as the recommended selector. Comments are context, not state." },
    { "comment", "Free-form note attached to a plan, phase, or task.", "id, entity_type, entity_id, comment_type, body, author, created_at, updated_at, invalidated_at, invalidated_by.", "Comments do not change entity state. Invalidated comments remain queryable for history.", "comment.add, comment.list, comment.update, comment.invalidate, plan.comment_add, phase.comment_add, task.comment_add.", "Use comments for observations, decisions, and progress notes. Use state-changing operations for workflow changes." },
    { "instruction", "Durable guidance attached to a plan, phase, or task.", "id, entity_type, entity_id, instruction_type, body, author, created_at, updated_at, invalidated_at, invalidated_by.", "Instructions represent active operating guidance. Invalidated instructions remain queryable for history but are excluded from normal lists.", "instruction.add, instruction.list, instruction.update, instruction.invalidate.", "Use instructions for constraints and standing guidance agents should read before acting; use comments for conversational history." },
    { "closure_record", "Durable memory produced when work closes.", "id, entity_type, entity_id, closure_status, resolution, outcome_summary, closing_comment, lessons_learned, open_items_summary, followup_needed, author, event_id.", "Closure records are linked to events and preserve handoff memory after terminal operations.", "closure.get, task.close, task.cancel, task.replace, task.mark_duplicate, phase.close.", "Use closure.get before resuming old work or reconstructing why something ended." },
    { "event", "Append-only audit record for meaningful changes.", "id, entity_type, entity_id, event_type, actor, event_at, summary, details, old_value, new_value, related entity, request_id.", "Events are emitted by semantic operations and retain request_id for caller correlation.", "event.list, plan.history, phase.history.", "Use history operations for audit and handoff instead of inferring history from current rows." },
    { "task_relation", "Directed relationship between tasks.", "id, from_task_id, to_task_id, relation_type, created_at, created_by, notes.", "Relation type is controlled. Replacement links are represented as replaces relations; dependency operations guard against cycles.", "task.link_dependency, task.unlink_dependency, task.replace, task.mark_duplicate.", "Use relations for dependencies, duplicates, replacements, and related tasks; do not encode these only in comments." },
    { "workspace_context", "Workspace-level focus state for agent handoff.", "workspace_context.active_plan_id plus per-plan current_phase_id/current_task_id in plan_contexts.", "The active plan is global to the workspace. Current phase/task are explicit per-plan cursors and are not inferred from statuses.", "workspace.context_get, plan.activate, plan.deactivate, phase.set_current, phase.clear_current, task.set_current, task.clear_current.", "Use context operations to resume work; use plan/phase/task status operations for workflow state." },
};

static char *xstrdup(const char *s) {
    size_t len = strlen(s);
    char *copy = malloc(len + 1);
    if (copy == NULL) return NULL;
    memcpy(copy, s, len + 1);
    return copy;
}

static int buf_reserve(Buf *b, size_t extra) {
    if (extra > SIZE_MAX - b->len - 1) return -1;
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return 0;
    size_t cap = b->cap == 0 ? 4096 : b->cap;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) return -1;
        cap *= 2;
    }
    char *next = realloc(b->data, cap);
    if (next == NULL) return -1;
    b->data = next;
    b->cap = cap;
    return 0;
}

static int buf_append_len(Buf *b, const char *s, size_t len) {
    if (buf_reserve(b, len) != 0) return -1;
    memcpy(b->data + b->len, s, len);
    b->len += len;
    b->data[b->len] = '\0';
    return 0;
}

static int buf_append(Buf *b, const char *s) {
    return buf_append_len(b, s, strlen(s));
}

static int buf_appendf(Buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list copy;
    va_copy(copy, ap);
    int needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(ap);
        return -1;
    }
    if (buf_reserve(b, (size_t)needed) != 0) {
        va_end(ap);
        return -1;
    }
    vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    b->len += (size_t)needed;
    return 0;
}

static char *buf_take(Buf *b) {
    if (b->data == NULL) return xstrdup("");
    char *data = b->data;
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    return data;
}

static uint64_t fnv1a_update(uint64_t hash, const char *data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        hash ^= (unsigned char)data[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void hash_hex(uint64_t hash, char out[17]) {
    snprintf(out, 17, "%016llx", (unsigned long long)hash);
}

static void content_hash_hex(const char *s, char out[17]) {
    hash_hex(fnv1a_update(UINT64_C(1469598103934665603), s, strlen(s)), out);
}

static int artifact_add(ArtifactList *list, const char *rel_path, char *content) {
    if (content == NULL) return -1;
    if (list->len == list->cap) {
        size_t next_cap = list->cap == 0 ? 64 : list->cap * 2;
        GeneratedArtifact *next = realloc(list->items, next_cap * sizeof next[0]);
        if (next == NULL) {
            return -1;
        }
        list->items = next;
        list->cap = next_cap;
    }
    GeneratedArtifact *item = &list->items[list->len++];
    item->rel_path = xstrdup(rel_path);
    item->content = content;
    if (item->rel_path == NULL) return -1;
    content_hash_hex(content, item->hash_hex);
    return 0;
}

static void artifacts_free(ArtifactList *list) {
    for (size_t i = 0; i < list->len; ++i) {
        free(list->items[i].rel_path);
        free(list->items[i].content);
    }
    free(list->items);
}

static int path_join(char *out, size_t outlen, const char *home, const char *rel) {
    int n = snprintf(out, outlen, "%s/%s", home, rel);
    return n >= 0 && (size_t)n < outlen ? 0 : -1;
}

static int ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return S_ISDIR(st.st_mode) ? 0 : -1;
    if (errno != ENOENT) return -1;
    if (mkdir(path, 0700) != 0) return -1;
    return chmod(path, 0700) == 0 ? 0 : -1;
}

static int ensure_subdirs(const char *home) {
    static const char *dirs[] = {
        "protocol", "concepts", "entities", "operations",
        "examples", "workflows", "indexes", "schemas", "guides",
    };
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; ++i) {
        char path[PATH_MAX];
        if (path_join(path, sizeof path, home, dirs[i]) != 0 ||
            ensure_dir(path) != 0) {
            return -1;
        }
    }
    return 0;
}

static int read_file(const char *path, char **content_out, size_t *len_out) {
    *content_out = NULL;
    *len_out = 0;
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return errno == ENOENT ? 1 : -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long len = ftell(f);
    if (len < 0) {
        fclose(f);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    char *buf = malloc((size_t)len + 1);
    if (buf == NULL) {
        fclose(f);
        return -1;
    }
    size_t got = fread(buf, 1, (size_t)len, f);
    if (got != (size_t)len || ferror(f)) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);
    buf[got] = '\0';
    *content_out = buf;
    *len_out = got;
    return 0;
}

static int atomic_write_file(const char *path, const char *content) {
    char tmp[PATH_MAX];
    int n = snprintf(tmp, sizeof tmp, "%s.tmp.%ld", path, (long)getpid());
    if (n < 0 || (size_t)n >= sizeof tmp) return -1;
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) return -1;
    size_t len = strlen(content);
    int ok = fwrite(content, 1, len, f) == len && fflush(f) == 0;
    if (fclose(f) != 0) ok = 0;
    if (!ok) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    chmod(path, 0600);
    return 0;
}

static int write_if_changed(const char *home, const GeneratedArtifact *artifact,
                            int *written_out) {
    char path[PATH_MAX];
    if (path_join(path, sizeof path, home, artifact->rel_path) != 0) return -1;
    char *old = NULL;
    size_t old_len = 0;
    int rc = read_file(path, &old, &old_len);
    if (rc == 0 && old_len == strlen(artifact->content) &&
        memcmp(old, artifact->content, old_len) == 0) {
        free(old);
        *written_out = 0;
        return 0;
    }
    free(old);
    if (rc < 0) return -1;
    if (atomic_write_file(path, artifact->content) != 0) return -1;
    *written_out = 1;
    return 0;
}

static void current_time_utc(char out[32]) {
    time_t now = time(NULL);
    struct tm tm_now;
    gmtime_r(&now, &tm_now);
    strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tm_now);
}

static void source_fingerprint(char out[17]) {
    uint64_t h = UINT64_C(1469598103934665603);
    h = fnv1a_update(h, IPMAN_AGENT_DOCS_GENERATOR_VERSION,
                     strlen(IPMAN_AGENT_DOCS_GENERATOR_VERSION));
    h = fnv1a_update(h, IPMAN_AGENT_DOCS_CONTENT_REVISION,
                     strlen(IPMAN_AGENT_DOCS_CONTENT_REVISION));
    size_t count = ipman_dispatch_operation_count();
    for (size_t i = 0; i < count; ++i) {
        const char *name = ipman_dispatch_operation_name_at(i);
        h = fnv1a_update(h, name, strlen(name));
    }
    hash_hex(h, out);
}

static int load_reusable_generated_at(const char *home,
                                      const char *fingerprint,
                                      char generated_at[32]) {
    char path[PATH_MAX];
    if (path_join(path, sizeof path, home, "manifest.json") != 0) return 0;
    char *content = NULL;
    size_t len = 0;
    if (read_file(path, &content, &len) != 0) return 0;
    (void)len;
    cJSON *root = cJSON_Parse(content);
    free(content);
    if (root == NULL) return 0;
    cJSON *fp = cJSON_GetObjectItemCaseSensitive(root, "source_fingerprint");
    cJSON *ga = cJSON_GetObjectItemCaseSensitive(root, "generated_at");
    int ok = cJSON_IsString(fp) && cJSON_IsString(ga) &&
             strcmp(fp->valuestring, fingerprint) == 0 &&
             strlen(ga->valuestring) < 32;
    if (ok) {
        snprintf(generated_at, 32, "%s", ga->valuestring);
    }
    cJSON_Delete(root);
    return ok;
}

static int append_frontmatter(Buf *b, const char *kind, const char *generated_at,
                              const char *subject_key, const char *subject,
                              const char *entity) {
    if (buf_appendf(b,
                    "---\nkind: %s\nprotocol_version: 1\n"
                    "generator_version: %s\ngenerated_at: %s\n",
                    kind, IPMAN_AGENT_DOCS_GENERATOR_VERSION, generated_at) != 0) {
        return -1;
    }
    if (subject_key != NULL && subject != NULL &&
        buf_appendf(b, "%s: %s\n", subject_key, subject) != 0) return -1;
    if (entity != NULL && buf_appendf(b, "entity: %s\n", entity) != 0) return -1;
    return buf_append(b, "do_not_edit: true\n---\n\n");
}

static char *render_start_here(const char *home, const char *db_path,
                               const char *generated_at) {
    Buf b = {0};
    if (append_frontmatter(&b, "start_here", generated_at, NULL, NULL, NULL) != 0) return NULL;
    if (buf_appendf(&b,
        "# START HERE\n\n"
        "`ipman` is a local JSON-in / JSON-out plan manager for agents and scripts. "
        "It stores structured plans, phases, tasks, instructions, comments, closure memory, relations, and audit events in SQLite.\n\n"
        "- Effective workspace: `%s`\n"
        "- Database: `%s`\n"
        "- Manifest: `%s/manifest.json`\n\n"
        "Invoke it by sending one JSON request on stdin and reading one JSON response on stdout:\n\n"
        "```sh\n"
        "printf '%%s' '{\"protocol_version\":1,\"request_id\":\"agent-noop\",\"actor\":\"agent\",\"op\":\"noop\",\"params\":{}}' | ipman\n"
        "```\n\n"
        "Use `ipman b64` or `ipman --b64` when shell escaping is awkward; stdin is base64-encoded JSON and stdout is a base64-encoded JSON response.\n\n"
        "Stdout is the API surface and must be parsed as JSON after any base64 transport decoding. Logs go to stderr. Always inspect `.ok`; on failure inspect `.error.code` and `.error.message`.\n\n"
        "## Common Intents\n\n"
        "- Create a plan: `%s/operations/ipman.op.plan.create.schema.md` and `%s/examples/ipman.op.plan.create.input.example.md`\n"
        "- List pending work: `%s/operations/ipman.op.task.list.schema.md`\n"
        "- Start work: `%s/operations/ipman.op.task.transition.schema.md`\n"
        "- Defer work: `%s/operations/ipman.op.task.defer.schema.md`\n"
        "- Cancel work: `%s/operations/ipman.op.task.cancel.schema.md`\n"
        "- Replace work: `%s/operations/ipman.op.task.replace.schema.md`\n"
        "- Close work: `%s/operations/ipman.op.task.close.schema.md`\n"
        "- Read history: `%s/operations/ipman.op.event.list.schema.md`, `%s/operations/ipman.op.plan.history.schema.md`, `%s/operations/ipman.op.phase.history.schema.md`\n"
        "- Recover closure memory: `%s/operations/ipman.op.closure.get.schema.md`\n\n"
        "## Warnings\n\n"
        "- Do not manually edit SQLite; use semantic operations.\n"
        "- Do not collapse `status`, `resolution`, and `origin_type`; they answer different questions.\n"
        "- Comments are context, not state transitions.\n"
        "- Prefer `task.defer`, `task.cancel`, `task.replace`, and `task.close` over generic updates for scope changes.\n\n",
        home, db_path, home,
        home, home, home, home, home, home, home, home, home, home, home, home) != 0) return NULL;
    if (buf_append(&b,
        "## Entity Identity\n\n"
        "Every entity carries multiple identifiers with distinct purposes:\n\n"
        "- **`uid`**: immutable system identifier assigned at creation (e.g. `task_42`, `phase_3`, `plan_1`). The preferred selector for automated references and agent handoffs — stable, unambiguous, and never reused.\n"
        "- **`label`**: semantic slug unique within its scope (e.g. `implement-login`). Preferred for comments and handoff notes because it is human-readable. Requires `plan_uid` or `plan_label` scope when used with task and phase selectors.\n"
        "- **`id`**: global numeric primary key. Convenient for round-tripping a value ipman just returned in the same session.\n"
        "- **`code`** (plans only): short human-readable plan identifier (e.g. `REL-001`, auto-assigned as `P<id>`).\n"
        "- **`local_seq`**: per-plan creation-order number, 1..N. Assigned at insert, **never reassigned**.\n"
        "- **`sequence_no`** (phases only): ordinal position within the plan, mutated by `phase.move`. Use for *ordering*, not for identity.\n\n"
        "Selector rules:\n\n"
        "1. Most phase and task operations accept `uid`, `id`, or `label`+scope. If more than one selector is supplied, runtime resolution uses `uid` first, then `id`, then `label`. `label` requires `plan_uid` or `plan_label` scope. `phase.list_tasks` also accepts `phase_id` as an alias for `id`; when both are supplied they must match.\n"
        "2. Most plan operations accept `uid`, `id`, `label`, or `code`. If more than one selector is supplied, runtime resolution uses `uid` first, then `id`, then `label`, then `code`. `plan.activate` is stricter and requires exactly one of `id` or `code`.\n"
        "3. Generated request JSON schemas express allowed params, primitive types/enums, required scalar fields, selector alternatives, and closure/update alternatives. Runtime validation remains the authority for database existence, state transitions, and cross-field business rules.\n"
        "4. Audit-side payloads carry `entity_ref` alongside numeric ids in `event.list`/`*.history` for display.\n\n"
        "## Workspace Context\n\n"
        "ipman maintains an explicit cursor for each workspace:\n\n"
        "- **Active plan**: the workspace-level cursor set by `plan.activate` and cleared by `plan.deactivate`, `plan.close`, or `plan.archive` on the active plan.\n"
        "- **Current phase** and **current task**: saved per plan. They are preserved when the plan is deactivated and restored when it is reactivated.\n\n"
        "Read the live cursor with `workspace.context_get`. Never infer it from `status='in_progress'` or recent timestamps.\n\n"
        "Invariants ipman enforces:\n\n"
        "1. Cursors always reference non-terminal entities, or are null. `phase.set_current` and `task.set_current` reject terminal targets.\n"
        "2. `task.set_current` auto-syncs the phase cursor to the task's phase; if the task has no phase, `current_phase` is cleared.\n"
        "3. Changing `current_phase` nulls `current_task` when the task does not belong to the new phase. A task without a phase survives any phase change.\n"
        "4. Terminating a cursor target clears the cursor automatically. Closing or canceling the `current_task` (via `task.close`, `task.cancel`, `task.mark_duplicate`, `task.replace`) nulls `current_task` in the same transaction; closing or canceling the `current_phase` does the same for `current_phase` and any incompatible `current_task`.\n"
        "5. Setting a cursor to its existing value is a no-op; no event is emitted.\n\n"
        "Consequence: after a terminal transition you do not need to call `task.clear_current` or `phase.clear_current` — those are for manually abandoning a still-open target.\n\n") != 0) return NULL;
    if (buf_appendf(&b,
        "## Discovery Paths\n\n"
        "- Machine entrypoint: `%s/manifest.json`\n"
        "- Protocol: `%s/protocol/ipman.protocol.envelope.md`\n"
        "- Operations index: `%s/indexes/ipman.index.operations.md`\n"
        "- Entity index: `%s/indexes/ipman.index.by-entity.md`\n"
        "- Workflow index: `%s/indexes/ipman.index.by-workflow.md`\n"
        "- Agent handoff workflow: `%s/workflows/ipman.workflow.agent-handoff.md`\n"
        "- Best practices guide: `%s/guides/ipman.guide.best-practices.md`\n",
        home, home, home, home, home, home, home) != 0) return NULL;
    return buf_take(&b);
}

static char *render_protocol_doc(const char *generated_at) {
    Buf b = {0};
    if (append_frontmatter(&b, "protocol", generated_at, "name", "ipman.protocol.envelope", NULL) != 0) return NULL;
    if (buf_append(&b,
        "# ipman Protocol Envelope\n\n"
        "Every invocation reads one JSON object from stdin and writes one JSON object to stdout. In `b64`/`--b64` transport mode, stdin and stdout carry base64-encoded JSON instead.\n\n"
        "## Request Fields\n\n"
        "- `protocol_version`: integer, currently `1`.\n"
        "- `request_id`: non-empty string echoed in the response when the request can be parsed far enough.\n"
        "- `actor`: non-empty string recorded in events, instructions, and comments.\n"
        "- `op`: non-empty public operation name from the operation index.\n"
        "- `params`: operation-specific object.\n\n"
        "## Success Response\n\n"
        "```json\n{\"request_id\":\"req-1\",\"ok\":true,\"result\":{}}\n```\n\n"
        "## Error Response\n\n"
        "```json\n{\"request_id\":\"req-1\",\"ok\":false,\"error\":{\"code\":\"validation_failed\",\"message\":\"title must be a non-empty string\"}}\n```\n\n"
        /* IMPORTANT: this list mirrors the strings in src/protocol.c:k_code_strings.
         * If you add or remove an error code in src/protocol.h, update both
         * places — the generator does not introspect the enum. */
        "Known error codes: `invalid_request`, `unknown_op`, `validation_failed`, `not_found`, `conflict`, `internal_error`.\n\n"
        "Semantic errors such as validation failures, conflicts, missing records, and unknown operations are JSON responses and usually exit 0. Fatal protocol and internal errors exit non-zero. Always parse stdout and inspect `.ok`.\n\n"
        "### Optional `error.details`\n\n"
        "`error.details` is an optional machine-readable object that some errors attach to the envelope. Agents can switch on `error.details.kind` for fine-grained branching without parsing `error.message`. When present, `code` and `message` keep their existing meaning — `details` is additive.\n\n"
        "Transition failures on task operations (`task.transition`, `task.defer`, `task.cancel`, `task.mark_duplicate`, `task.replace`, `task.close`, `task.reopen`) return `code: \"conflict\"` with:\n\n"
        "```json\n{\"code\":\"conflict\",\"message\":\"Transition from in_progress to deferred is not allowed\",\"details\":{\"kind\":\"invalid_state_transition\",\"entity_type\":\"task\",\"current_status\":\"in_progress\",\"attempted_status\":\"deferred\",\"allowed_next\":[{\"status\":\"todo\",\"operation\":\"task.transition\"},{\"status\":\"blocked\",\"operation\":\"task.transition\"},{\"status\":\"deferred\",\"operation\":\"task.defer\"},{\"status\":\"canceled\",\"operation\":\"task.cancel\"},{\"status\":\"canceled\",\"operation\":\"task.mark_duplicate\"},{\"status\":\"canceled\",\"operation\":\"task.replace\"},{\"status\":\"done\",\"operation\":\"task.close\"}],\"required_operation\":\"task.defer\"}}\n```\n\n"
        "- `current_status` / `attempted_status`: where the task is and where the call tried to take it.\n"
        "- `allowed_next[]`: every transition reachable from `current_status`, each paired with the operation that performs it.\n"
        "- `required_operation`: when the attempted transition exists but requires a specific semantic op, names that op; otherwise `null`.\n\n"
        "## Minimum Valid Example\n\n"
        "```json\n{\"protocol_version\":1,\"request_id\":\"min-1\",\"actor\":\"agent\",\"op\":\"noop\",\"params\":{}}\n```\n\n"
        "## Error Example\n\n"
        "```json\n{\"protocol_version\":1,\"request_id\":\"bad-1\",\"actor\":\"agent\",\"op\":\"plan.create\",\"params\":{}}\n```\n") != 0) return NULL;
    return buf_take(&b);
}

static char *render_concepts_doc(const char *generated_at) {
    Buf b = {0};
    if (append_frontmatter(&b, "concepts", generated_at, "name", "core", NULL) != 0) return NULL;
    if (buf_append(&b,
        "# Core Concepts\n\n"
        "- Plan: top-level work container.\n"
        "- Phase: ordered stage within a plan.\n"
        "- Task: operational unit of work.\n"
        "- Comment: note attached to a plan, phase, or task.\n"
        "- ClosureRecord: durable memory written by close/cancel/replace flows.\n"
        "- Event: audit record emitted by semantic operations.\n"
        "- TaskRelation: dependency, duplicate, replacement, or related-task link.\n\n"
        "`status` is not `resolution`, and neither is `origin_type`. `status` says where work is now. `resolution` explains why terminal work ended. `origin_type` explains why the task exists. Keeping them separate preserves scope history: a planned task can become canceled because it was replaced, while the replacement task has origin_type `replacement`.\n\n"
        "Use semantic operations so ipman can maintain events, closure records, relation rows, and invariants. Comments are not a substitute for state: a comment can explain why work changed, but `task.defer`, `task.cancel`, `task.replace`, and `task.close` record the actual state transition.\n") != 0) return NULL;
    return buf_take(&b);
}

static char *render_enums_doc(const char *generated_at) {
    Buf b = {0};
    if (append_frontmatter(&b, "enums", generated_at, "name", "ipman.enums", NULL) != 0) return NULL;
    if (buf_append(&b,
        "# Public Enums\n\n"
        "- Plan status: `open`, `in_progress`, `paused`, `completed`, `canceled`, `archived`.\n"
        "- Phase status: `open`, `in_progress`, `blocked`, `completed`, `canceled`.\n"
        "- Task status: `todo`, `in_progress`, `blocked`, `deferred`, `done`, `canceled`.\n"
        "- Task resolution: `completed`, `canceled`, `not_planned`, `replaced`, `duplicate`, `discarded`.\n"
        "- Priority: `low`, `medium`, `high`, `critical`.\n"
        "- Task type: `task`, `research`, `bug`, `decision`, `review`, `documentation`.\n"
        "- Origin type: `planned`, `addendum`, `discovered`, `replacement`, `carryover`, `external_request`.\n"
        "- Entity type: `plan`, `phase`, `task`.\n"
        "- Task relation type: `blocks`, `blocked_by`, `related`, `duplicates`, `replaces`, `parent_child`.\n"
        "- Dependency operation relation type: `blocks`, `blocked_by`, `related`, `duplicates`.\n"
        "- Event type: `plan_created`, `plan_updated`, `plan_closed`, `plan_archived`, `plan_reopened`, `plan_activated`, `plan_deactivated`, `phase_created`, `phase_updated`, `phase_moved`, `phase_closed`, `phase_reopened`, `phase_current_changed`, `task_created`, `task_updated`, `task_linked_external`, `task_status_changed`, `task_deferred`, `task_canceled`, `task_replaced`, `task_reopened`, `task_closed`, `task_current_changed`, `comment_added`, `comment_updated`, `comment_invalidated`, `instruction_added`, `instruction_updated`, `instruction_invalidated`.\n") != 0) return NULL;
    return buf_take(&b);
}

static char *render_entity_doc(const EntitySpec *spec, const char *generated_at) {
    Buf b = {0};
    if (append_frontmatter(&b, "entity_description", generated_at, "entity", spec->name, NULL) != 0) return NULL;
    if (buf_appendf(&b,
        "# Entity: %s\n\n"
        "## Purpose\n\n%s\n\n"
        "## Important Conceptual Fields\n\n%s\n\n"
        "## Invariants\n\n%s\n\n"
        "## Related Operations\n\n%s\n\n"
        "## Practical Guidance For Agents\n\n%s\n\n"
        "Common anti-patterns: editing SQLite manually, encoding state only in comments, and ignoring `.ok` on operation responses.\n",
        spec->name, spec->purpose, spec->fields, spec->invariants,
        spec->operations, spec->guidance) != 0) return NULL;
    return buf_take(&b);
}

static const char *selector_note_for_op(const char *name) {
    if (op_uses_task_selector(name) || op_uses_phase_selector(name)) {
        return "Selector contract: accepts `uid`, `id`, or `label` with `plan_uid` or `plan_label` scope. If multiple selectors are supplied, runtime resolution uses `uid` first, then `id`, then `label`.";
    }
    if (op_uses_phase_or_phase_id_selector(name)) {
        return "Selector contract: accepts `uid`, `id`, `phase_id`, or `label` with `plan_uid` or `plan_label` scope. `phase_id` is an alias for `id`; when supplied with `uid`, `id`, or scoped `label`, it must match the resolved selector. If multiple non-`phase_id` selectors are supplied, runtime resolution uses `uid` first, then `id`, then `label`.";
    }
    if (op_uses_plan_selector(name)) {
        return "Selector contract: accepts `uid`, `id`, `label`, or `code`. If multiple selectors are supplied, runtime resolution uses `uid` first, then `id`, then `label`, then `code`.";
    }
    if (strcmp(name, "plan.activate") == 0) {
        return "Selector contract: requires exactly one of `id` or `code`.";
    }
    return NULL;
}

static char *render_operation_doc(const OperationSpec *spec, const char *generated_at) {
    Buf b = {0};
    const char *selector_note = selector_note_for_op(spec->name);
    if (append_frontmatter(&b, "operation_schema", generated_at, "op", spec->name, spec->entity) != 0) return NULL;
    if (buf_appendf(&b,
        "# Operation: %s\n\n"
        "Primary entity: `%s`.\n\n"
        "## Purpose\n\n%s\n\n"
        "## When To Use It\n\nIntent: `%s`. %s\n\n"
        "## Params\n\n"
        "- Required: %s.\n"
        "- Optional: %s.\n\n"
        "%s%s"
        "## Expected Shape And Validations\n\n%s\n\n"
        "Public enum values are listed in `../concepts/ipman.enums.md` when this operation accepts controlled values.\n\n"
        "## Generated Request JSON Schema\n\n"
        "`../schemas/ipman.op.%s.request.schema.json` describes the machine-checkable request params for this operation. It covers allowed param names, primitive types/enums, required scalar fields, selector alternatives, and documented closure/update alternatives. Runtime validation remains the authority for database existence, state transitions, and cross-field business rules.\n\n"
        "## Preconditions\n\n%s\n\n"
        "## Side Effects\n\n%s\n\n"
        "## Common Errors\n\n%s\n\n"
        "## Success Response Shape\n\n%s\n\n"
        "## Output Fields\n\n%s\n\n"
        "## Related Operations\n\n%s\n\n"
        "## Important Notes\n\nDo not infer state changes from comments. Use this semantic operation when it matches the intent, then inspect `.ok` and `.error` in the response envelope.\n",
        spec->name, spec->entity, spec->summary, spec->intent, spec->summary,
        spec->required, spec->optional,
        selector_note ? selector_note : "",
        selector_note ? "\n\n" : "",
        spec->validations, spec->name, spec->preconditions,
        spec->side_effects, spec->errors, spec->response,
        spec->output_fields[0] ? spec->output_fields : "none",
        spec->related) != 0) return NULL;
    return buf_take(&b);
}

static char *render_example_doc(const OperationSpec *spec, const char *generated_at) {
    Buf b = {0};
    if (append_frontmatter(&b, "operation_example", generated_at, "op", spec->name, spec->entity) != 0) return NULL;
    if (buf_appendf(&b,
        "# Example: %s\n\n"
        "Adapt ids, codes, dates, and text to the current workspace before running. The envelope includes `request_id`, `actor`, `protocol_version`, `op`, and `params`.\n\n"
        "```json\n"
        "{\n"
        "  \"protocol_version\": 1,\n"
        "  \"request_id\": \"example-%s\",\n"
        "  \"actor\": \"agent\",\n"
        "  \"op\": \"%s\",\n"
        "  \"params\": %s\n"
        "}\n"
        "```\n",
        spec->name, spec->name, spec->name, spec->example_params) != 0) return NULL;
    return buf_take(&b);
}

static const ipman_param_desc_t *operation_params_for_name(const char *name) {
    size_t count = ipman_dispatch_operation_count();
    for (size_t i = 0; i < count; ++i) {
        const char *op_name = ipman_dispatch_operation_name_at(i);
        if (op_name != NULL && strcmp(op_name, name) == 0) {
            return ipman_dispatch_operation_params_at(i);
        }
    }
    return NULL;
}

static int is_name_in(const char *name, const char * const *names) {
    for (size_t i = 0; names[i] != NULL; ++i) {
        if (strcmp(name, names[i]) == 0) return 1;
    }
    return 0;
}

static int op_uses_phase_selector(const char *name) {
    static const char * const names[] = {
        "phase.close",
        "phase.comment_add",
        "phase.get",
        "phase.history",
        "phase.move",
        "phase.progress",
        "phase.reopen",
        "phase.update",
        NULL,
    };
    return is_name_in(name, names);
}

static int op_uses_phase_or_phase_id_selector(const char *name) {
    return strcmp(name, "phase.list_tasks") == 0;
}

static int op_uses_task_selector(const char *name) {
    static const char * const names[] = {
        "task.assign",
        "task.cancel",
        "task.close",
        "task.comment_add",
        "task.defer",
        "task.get",
        "task.link_dependency",
        "task.mark_duplicate",
        "task.move",
        "task.reopen",
        "task.replace",
        "task.set_origin",
        "task.set_priority",
        "task.set_type",
        "task.transition",
        "task.unassign",
        "task.unlink_dependency",
        "task.update",
        NULL,
    };
    return is_name_in(name, names);
}

static int op_uses_plan_selector(const char *name) {
    static const char * const names[] = {
        "plan.archive",
        "plan.close",
        "plan.get",
        "plan.history",
        "plan.progress",
        "plan.reopen",
        "plan.update",
        NULL,
    };
    return is_name_in(name, names);
}

static int op_requires_comment_or_closing_comment(const char *name) {
    static const char * const names[] = {
        "task.cancel",
        "task.mark_duplicate",
        "task.replace",
        NULL,
    };
    return is_name_in(name, names);
}

static int op_requires_defer_reason(const char *name) {
    return strcmp(name, "task.defer") == 0;
}

static int op_requires_one_plan_update(const char *name) {
    return strcmp(name, "plan.update") == 0;
}

static int op_requires_one_phase_update(const char *name) {
    return strcmp(name, "phase.update") == 0;
}

static int op_requires_one_task_update(const char *name) {
    return strcmp(name, "task.update") == 0;
}

static int append_required_rule(Buf *b, int *rule_count, const char *keyword,
                                const char * const *required_sets) {
    if (*rule_count == 0) {
        if (buf_append(b, ",\n      \"allOf\": [\n") != 0) return -1;
    } else if (buf_append(b, ",\n") != 0) {
        return -1;
    }
    if (buf_appendf(b, "        {\"%s\": [", keyword) != 0) return -1;
    for (size_t i = 0; required_sets[i] != NULL; ++i) {
        if (buf_appendf(b, "%s{\"required\": [%s]}",
                        i == 0 ? "" : ", ", required_sets[i]) != 0) {
            return -1;
        }
    }
    if (buf_append(b, "]}") != 0) return -1;
    ++*rule_count;
    return 0;
}

static int append_anyof_required_rule(Buf *b, int *rule_count,
                                      const char * const *required_sets) {
    return append_required_rule(b, rule_count, "anyOf", required_sets);
}

static int append_oneof_required_rule(Buf *b, int *rule_count,
                                      const char * const *required_sets) {
    return append_required_rule(b, rule_count, "oneOf", required_sets);
}

static int append_schema_rules(Buf *b, const OperationSpec *spec) {
    int rule_count = 0;

    if (op_uses_task_selector(spec->name) || op_uses_phase_selector(spec->name)) {
        static const char * const scoped_selector[] = {
            "\"uid\"",
            "\"id\"",
            "\"label\",\"plan_uid\"",
            "\"label\",\"plan_label\"",
            NULL,
        };
        if (append_anyof_required_rule(b, &rule_count, scoped_selector) != 0) {
            return -1;
        }
    } else if (op_uses_phase_or_phase_id_selector(spec->name)) {
        static const char * const phase_list_tasks_selector[] = {
            "\"uid\"",
            "\"id\"",
            "\"phase_id\"",
            "\"label\",\"plan_uid\"",
            "\"label\",\"plan_label\"",
            NULL,
        };
        if (append_anyof_required_rule(b, &rule_count,
                                       phase_list_tasks_selector) != 0) {
            return -1;
        }
    } else if (op_uses_plan_selector(spec->name)) {
        static const char * const plan_selector[] = {
            "\"uid\"",
            "\"id\"",
            "\"label\"",
            "\"code\"",
            NULL,
        };
        if (append_anyof_required_rule(b, &rule_count, plan_selector) != 0) {
            return -1;
        }
    } else if (strcmp(spec->name, "plan.activate") == 0) {
        static const char * const activate_selector[] = {
            "\"id\"",
            "\"code\"",
            NULL,
        };
        if (append_oneof_required_rule(b, &rule_count, activate_selector) != 0) {
            return -1;
        }
    }

    if (op_requires_comment_or_closing_comment(spec->name)) {
        static const char * const closure_text[] = {
            "\"closing_comment\"",
            "\"comment\"",
            NULL,
        };
        if (append_anyof_required_rule(b, &rule_count, closure_text) != 0) {
            return -1;
        }
    }

    if (op_requires_defer_reason(spec->name)) {
        static const char * const defer_reason[] = {
            "\"reason_code\"",
            "\"reason_text\"",
            NULL,
        };
        if (append_anyof_required_rule(b, &rule_count, defer_reason) != 0) {
            return -1;
        }
    }

    if (op_requires_one_plan_update(spec->name)) {
        static const char * const plan_updates[] = {
            "\"title\"", "\"summary\"", "\"description\"", "\"priority\"",
            "\"owner\"", "\"target_date\"", "\"tags\"", "\"version_label\"",
            NULL,
        };
        if (append_anyof_required_rule(b, &rule_count, plan_updates) != 0) {
            return -1;
        }
    } else if (op_requires_one_phase_update(spec->name)) {
        static const char * const phase_updates[] = {
            "\"title\"", "\"summary\"", "\"description\"", "\"owner\"",
            "\"target_start_date\"", "\"target_end_date\"",
            NULL,
        };
        if (append_anyof_required_rule(b, &rule_count, phase_updates) != 0) {
            return -1;
        }
    } else if (op_requires_one_task_update(spec->name)) {
        static const char * const task_updates[] = {
            "\"title\"", "\"summary\"", "\"description\"", "\"due_date\"",
            "\"target_start_date\"", "\"estimate\"", "\"blocked_reason\"",
            "\"reason_code\"", "\"reason_text\"",
            NULL,
        };
        if (append_anyof_required_rule(b, &rule_count, task_updates) != 0) {
            return -1;
        }
    }

    if (rule_count > 0 && buf_append(b, "\n      ]") != 0) return -1;
    return 0;
}

static int render_param_schema(Buf *b, const OperationSpec *spec,
                               const char *name) {
    (void)spec;
    if (strcmp(name, "id") == 0 ||
        strcmp(name, "entity_id") == 0 ||
        strcmp(name, "plan_id") == 0 ||
        strcmp(name, "sequence_no") == 0 ||
        strcmp(name, "parent_task_id") == 0 ||
        strcmp(name, "origin_task_id") == 0 ||
        strcmp(name, "target_task_id") == 0 ||
        strcmp(name, "since") == 0) {
        return buf_append(b, "{\"type\":\"integer\",\"minimum\":1}");
    }
    if (strcmp(name, "phase_id") == 0) {
        return buf_append(b, "{\"type\":[\"integer\",\"null\"],\"minimum\":1}");
    }
    if (strcmp(name, "limit") == 0) {
        return buf_append(b, "{\"type\":\"integer\",\"minimum\":1,\"maximum\":500}");
    }
    if (strcmp(name, "offset") == 0) {
        return buf_append(b, "{\"type\":\"integer\",\"minimum\":0}");
    }
    if (strcmp(name, "include_invalidated") == 0 ||
        strcmp(name, "completed_or_blocked") == 0 ||
        strcmp(name, "blocked") == 0 ||
        strcmp(name, "deferred") == 0 ||
        strcmp(name, "added_after_original") == 0 ||
        strcmp(name, "canceled") == 0 ||
        strcmp(name, "replaced") == 0 ||
        strcmp(name, "blocked_by_other") == 0 ||
        strcmp(name, "pending") == 0 ||
        strcmp(name, "followup_needed") == 0) {
        return buf_append(b, "{\"type\":\"boolean\"}");
    }
    if (strcmp(name, "tags") == 0) {
        return buf_append(b, "{\"type\":[\"array\",\"null\"],\"items\":{\"type\":\"string\"}}");
    }
    if (strcmp(name, "entity_type") == 0) {
        return buf_append(b, "{\"type\":\"string\",\"enum\":[\"plan\",\"phase\",\"task\"]}");
    }
    if (strcmp(name, "priority") == 0) {
        return buf_append(b, "{\"type\":[\"string\",\"null\"],\"enum\":[\"low\",\"medium\",\"high\",\"critical\",null]}");
    }
    if (strcmp(name, "task_type") == 0) {
        return buf_append(b, "{\"type\":\"string\",\"enum\":[\"task\",\"research\",\"bug\",\"decision\",\"review\",\"documentation\"]}");
    }
    if (strcmp(name, "origin_type") == 0) {
        return buf_append(b, "{\"type\":\"string\",\"enum\":[\"planned\",\"addendum\",\"discovered\",\"replacement\",\"carryover\",\"external_request\"]}");
    }
    if (strcmp(name, "resolution") == 0) {
        return buf_append(b, "{\"type\":[\"string\",\"null\"],\"enum\":[\"completed\",\"canceled\",\"not_planned\",\"replaced\",\"duplicate\",\"discarded\",null]}");
    }
    if (strcmp(name, "relation_type") == 0) {
        return buf_append(b, "{\"type\":\"string\",\"enum\":[\"blocks\",\"blocked_by\",\"related\",\"duplicates\"]}");
    }
    if (strcmp(name, "outcome") == 0) {
        return buf_append(b, "{\"type\":\"string\",\"enum\":[\"completed\",\"canceled\"]}");
    }
    if (strcmp(name, "ref_type") == 0) {
        return buf_append(b, "{\"type\":\"string\",\"enum\":[\"github_issue\",\"gitlab_issue\",\"commit\",\"pr\",\"adr\",\"url\",\"file\"]}");
    }
    if (strcmp(name, "status") == 0) {
        if (strncmp(spec->name, "plan.", 5) == 0) {
            return buf_append(b, "{\"type\":\"string\",\"enum\":[\"open\",\"in_progress\",\"paused\",\"completed\",\"canceled\",\"archived\"]}");
        }
        if (strncmp(spec->name, "phase.", 6) == 0) {
            return buf_append(b, "{\"type\":\"string\",\"enum\":[\"open\",\"in_progress\",\"blocked\",\"completed\",\"canceled\"]}");
        }
        return buf_append(b, "{\"type\":\"string\",\"enum\":[\"todo\",\"in_progress\",\"blocked\",\"deferred\",\"done\",\"canceled\"]}");
    }
    if (strcmp(name, "title") == 0 ||
        strcmp(name, "body") == 0 ||
        strcmp(name, "actor") == 0 ||
        strcmp(name, "request_id") == 0 ||
        strcmp(name, "op") == 0 ||
        strcmp(name, "uid") == 0 ||
        strcmp(name, "label") == 0 ||
        strcmp(name, "code") == 0 ||
        strcmp(name, "plan_uid") == 0 ||
        strcmp(name, "plan_label") == 0 ||
        strcmp(name, "assignee") == 0 ||
        strcmp(name, "comment_type") == 0 ||
        strcmp(name, "instruction_type") == 0 ||
        strcmp(name, "closing_comment") == 0 ||
        strcmp(name, "comment") == 0 ||
        strcmp(name, "outcome_summary") == 0 ||
        strcmp(name, "reason_code") == 0 ||
        strcmp(name, "reason_text") == 0 ||
        strcmp(name, "relation_notes") == 0 ||
        strcmp(name, "ref_id") == 0 ||
        strcmp(name, "event_type") == 0 ||
        strcmp(name, "from") == 0 ||
        strcmp(name, "to") == 0) {
        return buf_append(b, "{\"type\":\"string\",\"minLength\":1}");
    }
    return buf_append(b, "{\"type\":[\"string\",\"null\"]}");
}

static char *render_operation_schema_json(const OperationSpec *spec) {
    Buf b = {0};
    const char *required = spec->schema_required[0] == '\0' ? "" : spec->schema_required;
    const ipman_param_desc_t *params = operation_params_for_name(spec->name);
    if (params == NULL) return NULL;
    if (buf_appendf(&b,
        "{\n"
        "  \"$schema\": \"https://json-schema.org/draft/2020-12/schema\",\n"
        "  \"title\": \"ipman %s request\",\n"
        "  \"type\": \"object\",\n"
        "  \"additionalProperties\": false,\n"
        "  \"required\": [\"protocol_version\", \"request_id\", \"actor\", \"op\", \"params\"],\n"
        "  \"properties\": {\n"
        "    \"protocol_version\": {\"const\": 1},\n"
        "    \"request_id\": {\"type\": \"string\", \"minLength\": 1},\n"
        "    \"actor\": {\"type\": \"string\", \"minLength\": 1},\n"
        "    \"op\": {\"const\": \"%s\"},\n"
        "    \"params\": {\n"
        "      \"type\": \"object\",\n"
        "      \"description\": \"%s\",\n"
        "      \"required\": [%s],\n"
        "      \"additionalProperties\": false,\n"
        "      \"properties\": {\n",
        spec->name, spec->name, spec->summary, required) != 0) return NULL;
    for (const ipman_param_desc_t *p = params; p->name != NULL; ++p) {
        const ipman_param_desc_t *next = p + 1;
        if (buf_appendf(&b, "        \"%s\": ", p->name) != 0 ||
            render_param_schema(&b, spec, p->name) != 0 ||
            buf_appendf(&b, "%s\n", next->name != NULL ? "," : "") != 0) {
            return NULL;
        }
    }
    if (buf_append(&b, "      }") != 0) return NULL;
    if (append_schema_rules(&b, spec) != 0) return NULL;
    if (buf_append(
        &b,
        "\n"
        "    }\n"
        "  }\n"
        "}\n") != 0) return NULL;
    return buf_take(&b);
}

static char *render_envelope_schema_json(void) {
    return xstrdup(
        "{\n"
        "  \"$schema\": \"https://json-schema.org/draft/2020-12/schema\",\n"
        "  \"title\": \"ipman protocol envelope\",\n"
        "  \"type\": \"object\",\n"
        "  \"additionalProperties\": false,\n"
        "  \"required\": [\"protocol_version\", \"request_id\", \"actor\", \"op\", \"params\"],\n"
        "  \"properties\": {\n"
        "    \"protocol_version\": {\"const\": 1},\n"
        "    \"request_id\": {\"type\": \"string\", \"minLength\": 1},\n"
        "    \"actor\": {\"type\": \"string\", \"minLength\": 1},\n"
        "    \"op\": {\"type\": \"string\", \"minLength\": 1},\n"
        "    \"params\": {\"type\": \"object\"}\n"
        "  }\n"
        "}\n");
}

static char *render_manifest_schema_json(void) {
    return xstrdup(
        "{\n"
        "  \"$schema\": \"https://json-schema.org/draft/2020-12/schema\",\n"
        "  \"title\": \"ipman agent knowledge manifest\",\n"
        "  \"type\": \"object\",\n"
        "  \"required\": [\"generator_version\", \"protocol_version\", \"generated_at\", \"workspace_root\", \"db_path\", \"entrypoints\", \"indexes\", \"entities\", \"operations\", \"files\", \"source_fingerprint\", \"content_fingerprint\"],\n"
        "  \"properties\": {\n"
        "    \"generator_version\": {\"type\": \"string\"},\n"
        "    \"protocol_version\": {\"const\": 1},\n"
        "    \"generated_at\": {\"type\": \"string\"},\n"
        "    \"workspace_root\": {\"type\": \"string\"},\n"
        "    \"db_path\": {\"type\": \"string\"},\n"
        "    \"entrypoints\": {\"type\": \"object\"},\n"
        "    \"indexes\": {\"type\": \"object\"},\n"
        "    \"entities\": {\"type\": \"array\"},\n"
        "    \"operations\": {\"type\": \"array\"},\n"
        "    \"files\": {\"type\": \"array\", \"items\": {\"type\": \"string\"}},\n"
        "    \"source_fingerprint\": {\"type\": \"string\"},\n"
        "    \"content_fingerprint\": {\"type\": \"string\"}\n"
        "  }\n"
        "}\n");
}

static char *render_workflow_doc(const char *name, const char *generated_at) {
    Buf b = {0};
    if (append_frontmatter(&b, "workflow", generated_at, "workflow", name, NULL) != 0) return NULL;
    if (strcmp(name, "agent-handoff") == 0) {
        if (buf_append(&b,
            "# Workflow: Agent Handoff\n\n"
            "Objective: let a new agent recover current scope, pending work, decisions, and closure memory quickly.\n\n"
            "When to use it: at the start of a session, before changing scope, or before resuming another agent's work.\n\n"
            "Recommended sequence: `plan.list` to find active plans, `instruction.list` for standing guidance, `task.list` with `pending:true` for open work, `event.list` or `plan.history` for recent changes, `comment.list` for current notes, and `closure.get` for terminal memory.\n\n"
            "Continue work by moving a task with `task.transition`, recording progress with `task.comment_add`, and using `task.defer`, `task.cancel`, `task.replace`, or `task.close` for semantic scope changes.\n\n"
            "Common mistakes: relying only on comments, ignoring `.ok`, manually editing the database, and treating `status`, `resolution`, and `origin_type` as interchangeable.\n\n"
            "Minimal example:\n\n"
            "```json\n{\"protocol_version\":1,\"request_id\":\"handoff-pending\",\"actor\":\"agent\",\"op\":\"task.list\",\"params\":{\"pending\":true,\"limit\":100}}\n```\n") != 0) return NULL;
    } else if (strcmp(name, "release-management") == 0) {
        if (buf_append(&b,
            "# Workflow: Release Management\n\n"
            "Objective: track release scope from planning through closure.\n\n"
            "Use `plan.create`, `phase.create`, and `task.create` to model the release. Inspect with `plan.progress`, `phase.progress`, and `task.list`. Continue by transitioning tasks to `in_progress`, closing completed work with `task.close`, deferring out-of-release work with `task.defer`, and replacing changed scope with `task.replace`.\n\n"
            "Close with `plan.close` after release tasks are terminal, then archive with `plan.archive` when the plan should leave active views.\n\n"
            "Common mistakes: using comments to cancel work, closing without closure memory, or archiving before plan closure.\n\n"
            "```json\n{\"protocol_version\":1,\"request_id\":\"release-plan\",\"actor\":\"agent\",\"op\":\"plan.create\",\"params\":{\"code\":\"REL-001\",\"title\":\"Release 1\",\"priority\":\"high\"}}\n```\n") != 0) return NULL;
    } else if (strcmp(name, "bug-triage") == 0) {
        if (buf_append(&b,
            "# Workflow: Bug Triage\n\n"
            "Objective: capture bug reports, prioritize them, and preserve decisions.\n\n"
            "Create bugs with `task.create` and `task_type:\"bug\"`. Inspect with `task.list` filtered by `task_type` through the operation docs, add investigation notes with `task.comment_add`, link blockers with `task.link_dependency`, and use `task.cancel` or `task.mark_duplicate` when a report should not proceed.\n\n"
            "Continue by assigning ownership with `task.assign`, moving to `in_progress`, and closing with `task.close` when fixed and verified.\n\n"
            "Common mistakes: marking duplicates by text only, skipping closure memory, and losing the original origin metadata.\n\n"
            "```json\n{\"protocol_version\":1,\"request_id\":\"bug-task\",\"actor\":\"agent\",\"op\":\"task.create\",\"params\":{\"plan_id\":1,\"title\":\"Fix login failure\",\"task_type\":\"bug\",\"origin_type\":\"external_request\",\"priority\":\"high\"}}\n```\n") != 0) return NULL;
    } else {
        if (buf_append(&b,
            "# Workflow: Scope Control\n\n"
            "Objective: keep scope changes explicit and auditable.\n\n"
            "Inspect current work with `task.list`. Use `task.defer` for later work, `task.cancel` for removed work, `task.replace` when the correct work changed, and `task.close` only when the task is completed. Use `event.list` and `closure.get` to review why scope changed.\n\n"
            "Continue by creating addendum or discovered tasks with `task.create` and appropriate `origin_type` values.\n\n"
            "Common mistakes: editing `status` directly, using `task.update` for terminal decisions, or writing only a comment for a scope change.\n\n"
            "```json\n{\"protocol_version\":1,\"request_id\":\"scope-defer\",\"actor\":\"agent\",\"op\":\"task.defer\",\"params\":{\"id\":1,\"reason_text\":\"Out of current release scope.\"}}\n```\n") != 0) return NULL;
    }
    return buf_take(&b);
}

static char *render_operations_index(const char *home, const char *generated_at) {
    Buf b = {0};
    if (append_frontmatter(&b, "index", generated_at, "index", "operations", NULL) != 0) return NULL;
    if (buf_append(&b, "# Operations Index\n\n| Operation | Entity | Summary | Schema | Example |\n| --- | --- | --- | --- | --- |\n") != 0) return NULL;
    for (size_t i = 0; i < sizeof k_operation_specs / sizeof k_operation_specs[0]; ++i) {
        const OperationSpec *s = &k_operation_specs[i];
        if (buf_appendf(&b, "| `%s` | `%s` | %s | `%s/operations/ipman.op.%s.schema.md` | `%s/examples/ipman.op.%s.input.example.md` |\n",
                        s->name, s->entity, s->summary, home, s->name, home, s->name) != 0) return NULL;
    }
    return buf_take(&b);
}

static char *render_by_entity_index(const char *generated_at) {
    Buf b = {0};
    if (append_frontmatter(&b, "index", generated_at, "index", "by-entity", NULL) != 0) return NULL;
    if (buf_append(&b, "# Index By Entity\n\n") != 0) return NULL;
    for (size_t e = 0; e < sizeof k_entity_specs / sizeof k_entity_specs[0]; ++e) {
        const char *entity = k_entity_specs[e].name;
        if (buf_appendf(&b, "## %s\n\n", entity) != 0) return NULL;
        for (size_t i = 0; i < sizeof k_operation_specs / sizeof k_operation_specs[0]; ++i) {
            if (strcmp(k_operation_specs[i].entity, entity) == 0 &&
                buf_appendf(&b, "- `%s`: %s\n", k_operation_specs[i].name,
                            k_operation_specs[i].summary) != 0) return NULL;
        }
        if (buf_append(&b, "\n") != 0) return NULL;
    }
    return buf_take(&b);
}

static char *render_by_workflow_index(const char *generated_at) {
    static const char *intents[] = {
        "create/bootstrap", "inspect", "ongoing execution", "defer",
        "cancel", "replace", "close", "audit/history", "handoff",
    };
    Buf b = {0};
    if (append_frontmatter(&b, "index", generated_at, "index", "by-workflow", NULL) != 0) return NULL;
    if (buf_append(&b, "# Index By Workflow\n\n") != 0) return NULL;
    for (size_t intent = 0; intent < sizeof intents / sizeof intents[0]; ++intent) {
        if (buf_appendf(&b, "## %s\n\n", intents[intent]) != 0) return NULL;
        for (size_t i = 0; i < sizeof k_operation_specs / sizeof k_operation_specs[0]; ++i) {
            const OperationSpec *s = &k_operation_specs[i];
            int match = strcmp(s->intent, intents[intent]) == 0 ||
                        (strcmp(intents[intent], "handoff") == 0 &&
                         (strcmp(s->name, "closure.get") == 0 ||
                          strcmp(s->name, "event.list") == 0 ||
                          strcmp(s->name, "task.list") == 0 ||
                          strcmp(s->name, "comment.list") == 0)) ||
                        (strcmp(intents[intent], "replace") == 0 &&
                         strcmp(s->name, "task.replace") == 0);
            if (match && buf_appendf(&b, "- `%s`: %s\n", s->name, s->summary) != 0) return NULL;
        }
        if (buf_append(&b, "\n") != 0) return NULL;
    }
    return buf_take(&b);
}

static int artifact_path_for_op(char *out, size_t outlen, const char *dir,
                                const char *op, const char *suffix) {
    int n = snprintf(out, outlen, "%s/ipman.op.%s.%s", dir, op, suffix);
    return n >= 0 && (size_t)n < outlen ? 0 : -1;
}

static char *render_best_practices_doc(const char *generated_at) {
    Buf b = {0};
    if (append_frontmatter(&b, "guide", generated_at, "guide", "best-practices", NULL) != 0) return NULL;
    if (buf_append(&b,
        "# ipman Best Practices\n\n"
        "Ten recommendations for using ipman effectively, derived from expert guidance and documented usage patterns.\n\n"
        "## 1. Prefer uid and label over id for durable references\n\n"
        "`uid` (e.g. `plan_1`, `task_42`) is immutable and assigned at creation. "
        "`label` is a human-readable slug (e.g. `implement-login`) unique per plan scope. "
        "Use `uid` in automated cross-session references and agent handoffs. "
        "Use `label` (with `plan_uid` or `plan_label` scope) in comments and conversation. "
        "Reserve numeric `id` only for round-tripping values ipman just returned. "
        "When multiple selectors are accepted and supplied, ipman resolves task/phase selectors as `uid`, then `id`, then scoped `label`, and plan selectors as `uid`, then `id`, then `label`, then `code`. "
        "`plan.activate` is intentionally stricter and requires exactly one of `id` or `code`.\n\n"
        "## 2. Treat status, resolution, and origin_type as independent dimensions\n\n"
        "`status` records current workflow position (where the task is now). "
        "`resolution` records the terminal reason (why work ended). "
        "`origin_type` records why the task exists (planned, discovered, addendum, replacement, carryover, external_request). "
        "Never collapse them: a task can be `canceled` (status) with resolution `duplicate` because it is `discovered` (origin_type). "
        "Use `task.list` filters on all three independently.\n\n"
        "## 3. Read closure records before resuming old work\n\n"
        "Every `task.close`, `task.cancel`, `task.replace`, and `phase.close` writes a durable closure record. "
        "Closure records preserve `outcome_summary`, `lessons_learned`, `open_items_summary`, and `followup_needed`. "
        "Call `closure.get` with the entity type and id before reopening, replacing, or picking up work that a previous agent or session closed. "
        "This is the primary handoff memory mechanism in ipman.\n\n") != 0) return NULL;
    if (buf_append(&b,
        "## 4. Follow the agent handoff sequence\n\n"
        "At the start of every session or scope change:\n\n"
        "1. `workspace.context_get` — recover active plan and current phase/task cursors.\n"
        "2. `task.list` with `pending:true` — find all non-terminal non-deferred work.\n"
        "3. `event.list` with a recent `from` offset — see what changed since the last session.\n"
        "4. `closure.get` on any recently closed entities — read handoff memory.\n\n"
        "Never infer cursor state from `status='in_progress'` or recent timestamps.\n\n"
        "## 5. Classify new tasks by origin_type at creation\n\n"
        "Set `origin_type` when creating tasks: "
        "`planned` for work that was in the original scope, "
        "`addendum` for scope expansions decided mid-execution, "
        "`discovered` for reactive tasks that emerged during work, "
        "`replacement` for tasks that supersede a canceled predecessor (use `task.replace` which sets this automatically), "
        "`carryover` for unfinished work brought forward from a closed phase or previous cycle, "
        "`external_request` for tasks arriving from outside the plan. "
        "Correct origin_type enables accurate `task.list` filtering and plan.export auditing.\n\n"
        "## 6. Model work as Plan → Phase → Task, not as a flat list\n\n"
        "Plans hold the high-level goal and lifecycle (open, in_progress, paused, completed, canceled, archived). "
        "Phases provide ordered stages inside a plan; close a phase with `phase.close` only after all its tasks are terminal. "
        "Tasks are the atomic unit of work; they carry `priority`, `task_type`, assignee, and estimates. "
        "Use phases to sequence delivery milestones rather than to group categories. "
        "Avoid deeply nested parent/child tasks; prefer sibling tasks with dependency links.\n\n") != 0) return NULL;
    if (buf_append(&b,
        "## 7. Use task.replace for scope changes, not task.update + comment\n\n"
        "When the correct work changes (new title, new approach, different scope), call `task.replace`. "
        "It atomically cancels the old task with `resolution=replaced`, creates the replacement with `origin_type=replacement`, "
        "writes a closure record, and links the two tasks with a `replaces` relation. "
        "The audit trail then shows exactly what was superseded and why. "
        "Using `task.update` for scope changes hides the decision in the diff of a field; using a comment alone loses it entirely.\n\n"
        "## 8. Export plans before closing or archiving\n\n"
        "Call `plan.export` before `plan.close` and before `plan.archive`. "
        "The export is a canonical JSON snapshot containing phases, tasks, instructions, comments, events, closures, and relations. "
        "Store the snapshot in version control or a handoff artifact. "
        "Plan export is read-only and emits no events, so it is safe to call at any time.\n\n"
        "## 9. Use the MCP server for agent integration\n\n"
        "The `mcp/ipman_mcp.py` server wraps ipman as an MCP tool provider. "
        "Set `$IPMAN_HOME` and `$IPMAN_BIN` in the MCP server environment. "
        "The server reads `$IPMAN_HOME/manifest.json` to discover operations and expose them as tools. "
        "Each `tools/call` proxies to the ipman binary and returns structured JSON content. "
        "When configuring Claude Desktop or another MCP client, point `IPMAN_HOME` at the workspace `.ipman` directory and `IPMAN_BIN` at the installed binary.\n\n"
        "## 10. Run ipman init freely — it is idempotent\n\n"
        "`ipman init` is safe to run on an already-initialized workspace. "
        "It applies any missing schema migrations incrementally and regenerates documentation files only when content has changed. "
        "Files with identical content are not rewritten. "
        "Running `ipman init` does not delete data, reset cursors, or modify business records. "
        "Run it after upgrading the binary to pick up new migrations and documentation.\n\n"
        "## 11. Initialize a git repository if the codebase does not have one\n\n"
        "If the working directory has no git repository (local or remote), initialize one with `git init`. "
        "Add the following to `.gitignore`:\n\n"
        "- `.ipman/ipman.db` — binary SQLite (or SQLCipher-encrypted) file with no meaningful diff.\n"
        "- `.ipman/keysalt` — 32-byte secret used to derive the encryption key. Committing it defeats the encryption: anyone reading the git history could rederive the key.\n"
        "- `.ipman/ipman.db.bak` — plaintext backup left by `ipman migrate-encrypt`. Large, redundant, and (by definition) unencrypted.\n\n"
        "Commit the generated markdown files under `.ipman/` and any `plan.export` JSON snapshots at key milestones. "
        "This ties plan state to code state: exports survive outside the DB, "
        "and the history of decisions is recoverable even if the workspace is lost.\n\n"
        "## 12. Write meaningful outcome_summary and lessons_learned when closing tasks\n\n"
        "The handoff memory mechanism in ipman depends entirely on closure record quality. "
        "`outcome_summary` should state what was delivered and what condition the work is in — not just 'done'. "
        "`lessons_learned` should capture what was non-obvious: a hidden constraint, a design decision that was reconsidered, "
        "a workaround that future work should be aware of. "
        "If there is nothing to say in `lessons_learned`, pass `null` explicitly rather than an empty string. "
        "A closure record filled with vague text is indistinguishable from no record at all — "
        "it defeats best practice 3 (read closure records before resuming old work) for every session that follows.\n") != 0) return NULL;
    return buf_take(&b);
}

static int build_artifacts(const char *home, const char *db_path,
                           const char *generated_at, ArtifactList *list) {
    if (artifact_add(list, "START-HERE.md", render_start_here(home, db_path, generated_at)) != 0 ||
        artifact_add(list, "protocol/ipman.protocol.envelope.md", render_protocol_doc(generated_at)) != 0 ||
        artifact_add(list, "concepts/ipman.concepts.core.md", render_concepts_doc(generated_at)) != 0 ||
        artifact_add(list, "concepts/ipman.enums.md", render_enums_doc(generated_at)) != 0 ||
        artifact_add(list, "schemas/ipman.protocol.envelope.schema.json", render_envelope_schema_json()) != 0 ||
        artifact_add(list, "schemas/ipman.manifest.schema.json", render_manifest_schema_json()) != 0 ||
        artifact_add(list, "indexes/ipman.index.operations.md", render_operations_index(home, generated_at)) != 0 ||
        artifact_add(list, "indexes/ipman.index.by-entity.md", render_by_entity_index(generated_at)) != 0 ||
        artifact_add(list, "indexes/ipman.index.by-workflow.md", render_by_workflow_index(generated_at)) != 0 ||
        artifact_add(list, "guides/ipman.guide.best-practices.md", render_best_practices_doc(generated_at)) != 0) {
        return -1;
    }

    static const char *workflows[] = {
        "agent-handoff", "release-management", "bug-triage", "scope-control",
    };
    for (size_t i = 0; i < sizeof workflows / sizeof workflows[0]; ++i) {
        char rel[PATH_MAX];
        int n = snprintf(rel, sizeof rel, "workflows/ipman.workflow.%s.md", workflows[i]);
        if (n < 0 || (size_t)n >= sizeof rel ||
            artifact_add(list, rel, render_workflow_doc(workflows[i], generated_at)) != 0) {
            return -1;
        }
    }

    for (size_t i = 0; i < sizeof k_entity_specs / sizeof k_entity_specs[0]; ++i) {
        char rel[PATH_MAX];
        int n = snprintf(rel, sizeof rel, "entities/ipman.entity.%s.description.md",
                         k_entity_specs[i].name);
        if (n < 0 || (size_t)n >= sizeof rel ||
            artifact_add(list, rel, render_entity_doc(&k_entity_specs[i], generated_at)) != 0) {
            return -1;
        }
    }

    for (size_t i = 0; i < sizeof k_operation_specs / sizeof k_operation_specs[0]; ++i) {
        const OperationSpec *spec = &k_operation_specs[i];
        char rel[PATH_MAX];
        if (artifact_path_for_op(rel, sizeof rel, "operations", spec->name, "schema.md") != 0 ||
            artifact_add(list, rel, render_operation_doc(spec, generated_at)) != 0 ||
            artifact_path_for_op(rel, sizeof rel, "examples", spec->name, "input.example.md") != 0 ||
            artifact_add(list, rel, render_example_doc(spec, generated_at)) != 0 ||
            artifact_path_for_op(rel, sizeof rel, "schemas", spec->name, "request.schema.json") != 0 ||
            artifact_add(list, rel, render_operation_schema_json(spec)) != 0) {
            return -1;
        }
    }
    return 0;
}

static void all_content_fingerprint(const ArtifactList *list, char out[17]) {
    uint64_t h = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < list->len; ++i) {
        h = fnv1a_update(h, list->items[i].rel_path, strlen(list->items[i].rel_path));
        h = fnv1a_update(h, list->items[i].content, strlen(list->items[i].content));
    }
    hash_hex(h, out);
}

static char *public_path(const char *home, const char *rel) {
    Buf b = {0};
    if (buf_appendf(&b, "%s/%s", home, rel) != 0) return NULL;
    return buf_take(&b);
}

static int add_string_obj(cJSON *obj, const char *key, const char *value) {
    return cJSON_AddStringToObject(obj, key, value) != NULL ? 0 : -1;
}

static char *render_manifest(const char *home, const char *db_path,
                             const char *generated_at,
                             const char *source_fp,
                             const char *content_fp,
                             const ArtifactList *list) {
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return NULL;
    cJSON_AddStringToObject(root, "generator_version", IPMAN_AGENT_DOCS_GENERATOR_VERSION);
    cJSON_AddNumberToObject(root, "protocol_version", 1);
    cJSON_AddStringToObject(root, "generated_at", generated_at);
    cJSON_AddStringToObject(root, "workspace_root", home);
    cJSON_AddStringToObject(root, "db_path", db_path);
    cJSON_AddStringToObject(root, "source_fingerprint", source_fp);
    cJSON_AddStringToObject(root, "content_fingerprint", content_fp);

    cJSON *entrypoints = cJSON_AddObjectToObject(root, "entrypoints");
    cJSON *indexes = cJSON_AddObjectToObject(root, "indexes");
    cJSON *entities = cJSON_AddArrayToObject(root, "entities");
    cJSON *operations = cJSON_AddArrayToObject(root, "operations");
    cJSON *files = cJSON_AddArrayToObject(root, "files");
    cJSON *hashes = cJSON_AddObjectToObject(root, "file_hashes");
    if (entrypoints == NULL || indexes == NULL || entities == NULL ||
        operations == NULL || files == NULL || hashes == NULL) {
        cJSON_Delete(root);
        return NULL;
    }

    char *p = public_path(home, "START-HERE.md");
    if (p == NULL || add_string_obj(entrypoints, "start_here", p) != 0) { free(p); cJSON_Delete(root); return NULL; }
    free(p);
    p = public_path(home, "protocol/ipman.protocol.envelope.md");
    if (p == NULL || add_string_obj(entrypoints, "protocol", p) != 0) { free(p); cJSON_Delete(root); return NULL; }
    free(p);
    p = public_path(home, "indexes/ipman.index.operations.md");
    if (p == NULL || add_string_obj(entrypoints, "operations_index", p) != 0) { free(p); cJSON_Delete(root); return NULL; }
    free(p);
    p = public_path(home, "guides/ipman.guide.best-practices.md");
    if (p == NULL || add_string_obj(entrypoints, "best_practices", p) != 0) { free(p); cJSON_Delete(root); return NULL; }
    free(p);

    p = public_path(home, "indexes/ipman.index.by-entity.md");
    if (p == NULL || add_string_obj(indexes, "by_entity", p) != 0) { free(p); cJSON_Delete(root); return NULL; }
    free(p);
    p = public_path(home, "indexes/ipman.index.by-workflow.md");
    if (p == NULL || add_string_obj(indexes, "by_workflow", p) != 0) { free(p); cJSON_Delete(root); return NULL; }
    free(p);

    for (size_t i = 0; i < sizeof k_entity_specs / sizeof k_entity_specs[0]; ++i) {
        cJSON *item = cJSON_CreateObject();
        if (item == NULL) { cJSON_Delete(root); return NULL; }
        cJSON_AddStringToObject(item, "name", k_entity_specs[i].name);
        char rel[PATH_MAX];
        int n = snprintf(rel, sizeof rel, "entities/ipman.entity.%s.description.md", k_entity_specs[i].name);
        if (n < 0 || (size_t)n >= sizeof rel) { cJSON_Delete(item); cJSON_Delete(root); return NULL; }
        p = public_path(home, rel);
        if (p == NULL || add_string_obj(item, "doc", p) != 0) { free(p); cJSON_Delete(item); cJSON_Delete(root); return NULL; }
        free(p);
        cJSON_AddItemToArray(entities, item);
    }

    for (size_t i = 0; i < sizeof k_operation_specs / sizeof k_operation_specs[0]; ++i) {
        const OperationSpec *spec = &k_operation_specs[i];
        cJSON *item = cJSON_CreateObject();
        if (item == NULL) { cJSON_Delete(root); return NULL; }
        cJSON_AddStringToObject(item, "op", spec->name);
        cJSON_AddStringToObject(item, "entity", spec->entity);
        char rel[PATH_MAX];
        if (artifact_path_for_op(rel, sizeof rel, "operations", spec->name, "schema.md") != 0) { cJSON_Delete(item); cJSON_Delete(root); return NULL; }
        p = public_path(home, rel);
        if (p == NULL || add_string_obj(item, "schema_doc", p) != 0) { free(p); cJSON_Delete(item); cJSON_Delete(root); return NULL; }
        free(p);
        if (artifact_path_for_op(rel, sizeof rel, "examples", spec->name, "input.example.md") != 0) { cJSON_Delete(item); cJSON_Delete(root); return NULL; }
        p = public_path(home, rel);
        if (p == NULL || add_string_obj(item, "example_doc", p) != 0) { free(p); cJSON_Delete(item); cJSON_Delete(root); return NULL; }
        free(p);
        if (artifact_path_for_op(rel, sizeof rel, "schemas", spec->name, "request.schema.json") != 0) { cJSON_Delete(item); cJSON_Delete(root); return NULL; }
        p = public_path(home, rel);
        if (p == NULL || add_string_obj(item, "request_schema_json", p) != 0) { free(p); cJSON_Delete(item); cJSON_Delete(root); return NULL; }
        free(p);
        cJSON_AddItemToArray(operations, item);
    }

    p = public_path(home, "manifest.json");
    if (p == NULL) { cJSON_Delete(root); return NULL; }
    cJSON_AddItemToArray(files, cJSON_CreateString(p));
    free(p);
    for (size_t i = 0; i < list->len; ++i) {
        p = public_path(home, list->items[i].rel_path);
        if (p == NULL) { cJSON_Delete(root); return NULL; }
        cJSON_AddItemToArray(files, cJSON_CreateString(p));
        cJSON_AddStringToObject(hashes, p, list->items[i].hash_hex);
        free(p);
    }

    char *printed = cJSON_Print(root);
    cJSON_Delete(root);
    if (printed == NULL) return NULL;
    Buf b = {0};
    if (buf_append(&b, printed) != 0 || buf_append(&b, "\n") != 0) {
        cJSON_free(printed);
        free(b.data);
        return NULL;
    }
    cJSON_free(printed);
    return buf_take(&b);
}

int ipman_agent_docs_operation_spec_count(void) {
    return (int)(sizeof k_operation_specs / sizeof k_operation_specs[0]);
}

const char *ipman_agent_docs_operation_spec_name_at(int index) {
    if (index < 0 || (size_t)index >= sizeof k_operation_specs / sizeof k_operation_specs[0]) return NULL;
    return k_operation_specs[index].name;
}

/* Cached result of the most recent successful refresh in this process.
 * ipman_op_workspace_refresh_agent_docs reads from this so its reported
 * counts reflect the writes actually performed during this invocation,
 * rather than zero from a redundant second pass against just-written
 * files. ipman is one-shot per process, so cross-invocation staleness is
 * not a concern. */
static ipman_agent_docs_result_t g_last_refresh;
static int g_have_last_refresh;

int ipman_agent_docs_refresh(const char *home,
                            const char *db_path,
                            ipman_agent_docs_result_t *result_out) {
    if (home == NULL || db_path == NULL) return -1;
    if (ensure_subdirs(home) != 0) return -1;

    char source_fp[17];
    char generated_at[32];
    source_fingerprint(source_fp);
    if (!load_reusable_generated_at(home, source_fp, generated_at)) {
        current_time_utc(generated_at);
    }

    ArtifactList list = {0};
    if (build_artifacts(home, db_path, generated_at, &list) != 0) {
        artifacts_free(&list);
        return -1;
    }
    char content_fp[17];
    all_content_fingerprint(&list, content_fp);
    char *manifest = render_manifest(home, db_path, generated_at, source_fp,
                                     content_fp, &list);
    if (manifest == NULL ||
        artifact_add(&list, "manifest.json", manifest) != 0) {
        free(manifest);
        artifacts_free(&list);
        return -1;
    }

    int written = 0;
    int unchanged = 0;
    for (size_t i = 0; i < list.len; ++i) {
        int did_write = 0;
        if (write_if_changed(home, &list.items[i], &did_write) != 0) {
            artifacts_free(&list);
            return -1;
        }
        if (did_write) ++written;
        else ++unchanged;
    }

    ipman_agent_docs_result_t snapshot = {0};
    snapshot.files_total = (int)list.len;
    snapshot.files_written = written;
    snapshot.files_unchanged = unchanged;
    snprintf(snapshot.generated_at, sizeof snapshot.generated_at, "%s", generated_at);
    snprintf(snapshot.source_fingerprint, sizeof snapshot.source_fingerprint, "%s", source_fp);
    snprintf(snapshot.content_fingerprint, sizeof snapshot.content_fingerprint, "%s", content_fp);

    if (result_out != NULL) *result_out = snapshot;
    g_last_refresh = snapshot;
    g_have_last_refresh = 1;

    artifacts_free(&list);
    return 0;
}

const ipman_param_desc_t ipman_op_workspace_refresh_agent_docs_params[] = {
    { NULL },
};

int ipman_op_workspace_refresh_agent_docs(const ipman_request_t *req,
                                         sqlite3 *db,
                                         cJSON **result_out,
                                         ipman_error_code_t *err_code_out,
                                         const char **err_msg_out) {
    (void)req;
    (void)db;
    char home[PATH_MAX];
    if (ipman_home_resolve(home, sizeof home) != 0 ||
        ipman_home_require(home) != 0) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "cannot access initialized ipman home";
        return -1;
    }
    char db_path[PATH_MAX];
    int n = snprintf(db_path, sizeof db_path, "%s/ipman.db", home);
    if (n < 0 || (size_t)n >= sizeof db_path) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "db path too long";
        return -1;
    }
    /* In the normal JSON dispatch path, open_workspace already ran
     * ipman_agent_docs_refresh implicitly. Reuse that result so the
     * reported counts reflect what actually happened on this invocation
     * rather than a redundant second pass. Fall back to a fresh refresh
     * if the cache is unset (e.g. unit-test harnesses). */
    ipman_agent_docs_result_t refresh = {0};
    if (g_have_last_refresh) {
        refresh = g_last_refresh;
    } else if (ipman_agent_docs_refresh(home, db_path, &refresh) != 0) {
        ipman_log_error("agent docs refresh failed", "home=%s", home);
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to refresh agent docs";
        return -1;
    }
    cJSON *result = cJSON_CreateObject();
    if (result == NULL) {
        *err_code_out = IPMAN_ERR_INTERNAL;
        *err_msg_out = "failed to build refresh response";
        return -1;
    }
    cJSON_AddStringToObject(result, "workspace_root", home);
    cJSON_AddStringToObject(result, "db_path", db_path);
    cJSON_AddNumberToObject(result, "files_total", refresh.files_total);
    cJSON_AddNumberToObject(result, "files_written", refresh.files_written);
    cJSON_AddNumberToObject(result, "files_unchanged", refresh.files_unchanged);
    cJSON_AddStringToObject(result, "generated_at", refresh.generated_at);
    cJSON_AddStringToObject(result, "source_fingerprint", refresh.source_fingerprint);
    cJSON_AddStringToObject(result, "content_fingerprint", refresh.content_fingerprint);
    *result_out = result;
    return 0;
}
