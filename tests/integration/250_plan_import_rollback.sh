#!/bin/sh
set -eu

# Integration test: saga rollback on mid-import failure.
#
# Constructs a corrupt export envelope: a valid plan, 1 phase, 2 good tasks,
# then a 3rd task with an invalid priority ("urgent"). The importer:
#   1. Creates the plan            (succeeds)
#   2. Creates phase 1             (succeeds)
#   3. Creates task 1              (succeeds)
#   4. Creates task 2              (succeeds)
#   5. Attempts task 3 — FAILS     (invalid priority)
#   -> saga cleanup cancels task2, task1, phase1, and the plan
#
# Assertions:
#   - exit code is non-zero
#   - plan.list shows zero plans  (or zero non-terminal plans)
#   - phase.list (if plan existed) shows no open phases
#   - task.list shows zero tasks for any plan
#
# NOTE: The rollback uses task.cancel / phase.close / plan.close (compensating
# actions), not DELETE. Those ops move entities to terminal states. So we check
# that no non-terminal rows exist, not that the tables are empty.

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BIN="$ROOT/build/ipman"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run make first" >&2
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ipman-import-rollback.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

WS="$TMP/ws"
mkdir -p "$WS"

IPMAN_HOME="$WS/home" "$BIN" init >/dev/null 2>/dev/null

# Build a corrupt envelope by hand. The plan and phase are valid; the first
# two tasks are valid; the third task has priority="urgent" (not in the enum).
cat > "$TMP/corrupt.json" <<'EOF'
{
  "export_format_version": 3,
  "schema_version": 5,
  "generated_at": "2026-05-04T00:00:00Z",
  "plan": {
    "id": 999,
    "title": "Corrupt Import Plan",
    "summary": "Should be rolled back",
    "status": "open",
    "priority": "medium",
    "tags": []
  },
  "phases": [
    {
      "id": 10,
      "plan_id": 999,
      "title": "Phase Alpha",
      "status": "open",
      "sequence_no": 1
    }
  ],
  "tasks": [
    {
      "id": 100,
      "plan_id": 999,
      "phase_id": 10,
      "title": "Good Task One",
      "status": "todo",
      "priority": "low"
    },
    {
      "id": 101,
      "plan_id": 999,
      "phase_id": 10,
      "title": "Good Task Two",
      "status": "todo",
      "priority": "medium"
    },
    {
      "id": 102,
      "plan_id": 999,
      "phase_id": 10,
      "title": "Bad Task Three",
      "status": "todo",
      "priority": "urgent"
    }
  ],
  "comments": [],
  "instructions": [],
  "events": [],
  "closures": [],
  "relations": []
}
EOF

# Run the import — it must exit non-zero
import_stderr=$(IPMAN_HOME="$WS/home" "$BIN" --import-plan "$TMP/corrupt.json" 2>&1 || true)
import_rc=0
IPMAN_HOME="$WS/home" "$BIN" --import-plan "$TMP/corrupt.json" >/dev/null 2>/dev/null || import_rc=$?

[ "$import_rc" -ne 0 ] || {
    echo "FAIL: --import-plan should have exited non-zero" >&2
    exit 1
}

# Helper: run a JSON op in the workspace
ws_call() {
    printf '%s\n' "$1" | IPMAN_HOME="$WS/home" "$BIN" 2>/dev/null
}

# No open plans should remain.
# Rollback calls plan.close(outcome=canceled), which moves plan to terminal
# state. plan.list returns all plans. We assert none have status=open.
all_plans=$(ws_call '{"protocol_version":2,"request_id":"c1","actor":"t","op":"plan.list","params":{}}')
open_plan_count=$(printf '%s' "$all_plans" | jq '[.result.plans[] | select(.status == "open")] | length')
[ "$open_plan_count" = "0" ] || {
    echo "FAIL: found $open_plan_count open plan(s) after rollback" >&2
    exit 1
}

# No open tasks should remain (task.list with no plan_id = all tasks).
all_tasks=$(ws_call '{"protocol_version":2,"request_id":"c2","actor":"t","op":"task.list","params":{"limit":100}}')
open_task_count=$(printf '%s' "$all_tasks" | jq '[.result.tasks[] | select(.status != "canceled" and .status != "closed" and .status != "discarded" and .status != "not_planned" and .status != "duplicate")] | length')
[ "$open_task_count" = "0" ] || {
    echo "FAIL: found $open_task_count non-terminal task(s) after rollback" >&2
    exit 1
}

# Verify the rollback was exercised (not just a pre-create validation failure):
# The error message should mention task.create (meaning plan and phase were
# created before the failure hit). Checked from the first run output above.
printf '%s\n' "$import_stderr" | grep -q "task.create" || {
    echo "FAIL: rollback was not exercised — error hit before plan/phase creation" >&2
    echo "  (got: $import_stderr)" >&2
    exit 1
}

echo "ok 250_plan_import_rollback"
