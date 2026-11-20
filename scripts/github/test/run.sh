#!/usr/bin/env bash
# Host tests for scripts/github/setup-project.sh, driven by a `gh` double.
# No network, no credentials, no GitHub calls. Run: scripts/github/test/run.sh
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SCRIPT="$HERE/../setup-project.sh"
FAKE="$HERE/fake-gh"

pass=0; fail=0
ok()   { pass=$((pass+1)); printf '  ok   %s\n' "$1"; }
bad()  { fail=$((fail+1)); printf '  FAIL %s\n' "$1"; [[ -n "${2:-}" ]] && printf '       %s\n' "$2"; }
check() { if eval "$2"; then ok "$1"; else bad "$1" "assertion failed: $2"; fi; }

# --- fixture ----------------------------------------------------------------
new_mock() {
  MOCK_DIR="$(mktemp -d)"
  MOCK_LOG="$MOCK_DIR/calls.log"; : > "$MOCK_LOG"
  cat > "$MOCK_DIR/projects.json" <<'JSON'
{"projects":[{"number":1,"title":"JOS","id":"PID"}]}
JSON
  cat > "$MOCK_DIR/fields.json" <<'JSON'
{"fields":[
 {"id":"F_STATUS","name":"Status","type":"ProjectV2SingleSelectField","options":[
   {"id":"o_todo","name":"Todo"},{"id":"o_progress","name":"In progress"},
   {"id":"o_inreview","name":"In review"},{"id":"o_blocked","name":"Blocked"},
   {"id":"o_done","name":"Done"}]},
 {"id":"F_AREA","name":"Area","type":"ProjectV2SingleSelectField","options":[
   {"id":"a_firmware","name":"Firmware"},{"id":"a_comms","name":"COMMS"},
   {"id":"a_eps","name":"EPS"},{"id":"a_aocs","name":"AOCS"},
   {"id":"a_payloads","name":"Payloads"},{"id":"a_ci","name":"CI"},{"id":"a_docs","name":"Docs"}]},
 {"id":"F_PRIO","name":"Priority","type":"ProjectV2SingleSelectField","options":[
   {"id":"p_p0","name":"P0"},{"id":"p_p1","name":"P1"},{"id":"p_p2","name":"P2"}]},
 {"id":"F_ROW","name":"TASKS.md row","type":"ProjectV2Field","options":[]}
]}
JSON
  echo '{"items":[]}' > "$MOCK_DIR/items.json"
  echo '{"open_issues_count":0}' > "$MOCK_DIR/repo.json"
  echo '[]' > "$MOCK_DIR/prs.json"
  echo '[]' > "$MOCK_DIR/issues.json"
  echo '{}' > "$MOCK_DIR/meta.json"
}

run_script() {
  OUT="$(GH="$FAKE" MOCK_DIR="$MOCK_DIR" MOCK_LOG="$MOCK_LOG" \
        OWNER=Trisert REPO=Trisert/JOS TITLE=JOS "$SCRIPT" 2>&1)"
  RC=$?
}

issue() { # number state is_pr [label...]
  local n="$1" st="$2" pr="$3"; shift 3
  local labels='[]'
  (( $# > 0 )) && labels="$(printf '%s\n' "$@" | jq -R . | jq -s .)"
  jq --arg n "$n" --arg st "$st" --argjson pr "$pr" --argjson labels "$labels" \
     '.[$n] = {state:$st, is_pr:$pr, labels:$labels}' "$MOCK_DIR/meta.json" > "$MOCK_DIR/m" \
     && mv "$MOCK_DIR/m" "$MOCK_DIR/meta.json"
}

# --- T1: issue import with no open PRs (regression: pipefail abort) ---------
echo "T1 issue import with no open PRs"
new_mock
echo '[{"number":7,"url":"https://github.com/Trisert/JOS/issues/7"}]' > "$MOCK_DIR/issues.json"
issue 7 open false area/firmware prio/P1
run_script
check "exits 0"                '[[ $RC -eq 0 ]]'
check "imports the issue"      'grep -q "item-add 1 --owner Trisert --url https://github.com/Trisert/JOS/issues/7" "$MOCK_LOG"'
check "sets Area=Firmware"     'grep -q "field-id F_AREA --single-select-option-id a_firmware" "$MOCK_LOG"'
check "sets Priority=P1"       'grep -q "field-id F_PRIO --single-select-option-id p_p1" "$MOCK_LOG"'
check "status Todo"            'grep -q "field-id F_STATUS --single-select-option-id o_todo" "$MOCK_LOG"'
rm -rf "$MOCK_DIR"

# --- T2: PR without closing references --------------------------------------
echo "T2 open PR with no closing reference"
new_mock
cat > "$MOCK_DIR/prs.json" <<'JSON'
[{"number":2,"url":"https://github.com/Trisert/JOS/pull/2","body":"no references here"}]
JSON
issue 2 open true area/ci
run_script
check "exits 0"                '[[ $RC -eq 0 ]]'
check "PR is In review"        'grep -q "field-id F_STATUS --single-select-option-id o_inreview" "$MOCK_LOG"'
rm -rf "$MOCK_DIR"

# --- T3: Closes #N marks the issue In review --------------------------------
echo "T3 'Closes #7' marks issue 7 In review"
new_mock
echo '[{"number":7,"url":"https://github.com/Trisert/JOS/issues/7"}]' > "$MOCK_DIR/issues.json"
cat > "$MOCK_DIR/prs.json" <<'JSON'
[{"number":3,"url":"https://github.com/Trisert/JOS/pull/3","body":"Closes #7"}]
JSON
issue 7 open false area/payloads
issue 3 open true
run_script
check "exits 0"                      '[[ $RC -eq 0 ]]'
check "issue 7 gets In review"       '[[ $(grep -c "field-id F_STATUS --single-select-option-id o_inreview" "$MOCK_LOG") -eq 2 ]]'
rm -rf "$MOCK_DIR"

# --- T4: an unreadable item is not silently defaulted -----------------------
echo "T4 unreadable item state leaves the card untouched"
new_mock
echo '[{"number":9,"url":"https://github.com/Trisert/JOS/issues/9"}]' > "$MOCK_DIR/issues.json"
run_script
check "exits 0"                    '[[ $RC -eq 0 ]]'
check "warns about the item"       'grep -q "cannot read the state of #9" <<<"$OUT"'
check "sets no status"             '! grep -q "F_STATUS" "$MOCK_LOG"'
rm -rf "$MOCK_DIR"

# --- T4: a rejected field update must fail the run --------------------------
echo "T5 rejected field update is not silent"
new_mock
echo '[{"number":7,"url":"https://github.com/Trisert/JOS/issues/7"}]' > "$MOCK_DIR/issues.json"
issue 7 open false area/firmware
: > "$MOCK_DIR/fail-edits"
run_script
check "exits nonzero"        '[[ $RC -ne 0 ]]'
check "reports the failure"  'grep -q "field update failed" <<<"$OUT"'
check "names the count"      'grep -q "field update(s) failed" <<<"$OUT"'
rm -rf "$MOCK_DIR"

# --- T5: partially configured Status is reconciled --------------------------
echo "T6 missing Status option is added, existing ones kept"
new_mock
cat > "$MOCK_DIR/fields.json" <<'JSON'
{"fields":[
 {"id":"F_STATUS","name":"Status","type":"ProjectV2SingleSelectField","options":[
   {"id":"o_todo","name":"Todo"},{"id":"o_blocked","name":"Blocked"},{"id":"o_done","name":"Done"}]},
 {"id":"F_AREA","name":"Area","type":"ProjectV2SingleSelectField","options":[
   {"id":"a_firmware","name":"Firmware"},{"id":"a_comms","name":"COMMS"},{"id":"a_eps","name":"EPS"},
   {"id":"a_aocs","name":"AOCS"},{"id":"a_payloads","name":"Payloads"},{"id":"a_ci","name":"CI"},
   {"id":"a_docs","name":"Docs"}]},
 {"id":"F_PRIO","name":"Priority","type":"ProjectV2SingleSelectField","options":[
   {"id":"p_p0","name":"P0"},{"id":"p_p1","name":"P1"},{"id":"p_p2","name":"P2"}]},
 {"id":"F_ROW","name":"TASKS.md row","type":"ProjectV2Field","options":[]}
]}
JSON
cp "$HERE/fields-full.json" "$MOCK_DIR/fields-after.json"
run_script
check "exits 0"                          '[[ $RC -eq 0 ]]'
check "mutation sent"                    '[[ -f "$MOCK_DIR/graphql-input.json" ]]'
check "keeps existing Blocked option"    'jq -e "[.variables.options[].name] | index(\"Blocked\")" "$MOCK_DIR/graphql-input.json" >/dev/null'
check "adds In progress"                 'jq -e "[.variables.options[].name] | index(\"In progress\")" "$MOCK_DIR/graphql-input.json" >/dev/null'
check "adds In review"                   'jq -e "[.variables.options[].name] | index(\"In review\")" "$MOCK_DIR/graphql-input.json" >/dev/null'
check "no duplicate options"             'jq -e "([.variables.options[].name] | length) == ([.variables.options[].name] | unique | length)" "$MOCK_DIR/graphql-input.json" >/dev/null'
rm -rf "$MOCK_DIR"

# --- T6: idempotency --------------------------------------------------------
echo "T7 an item already on the board is not re-added"
new_mock
echo '[{"number":7,"url":"https://github.com/Trisert/JOS/issues/7"}]' > "$MOCK_DIR/issues.json"
cat > "$MOCK_DIR/items.json" <<'JSON'
{"items":[{"id":"ITEM_EXISTING","content":{"number":7,"url":"https://github.com/Trisert/JOS/issues/7"}}]}
JSON
issue 7 open false area/firmware
run_script
check "exits 0"          '[[ $RC -eq 0 ]]'
check "no item-add"      '! grep -q "item-add" "$MOCK_LOG"'
check "still sets fields" 'grep -q -- "--id ITEM_EXISTING" "$MOCK_LOG"'
rm -rf "$MOCK_DIR"

# --- T7: a project that does not exist yet is created -----------------------
echo "T8 missing project is created and its fields added"
new_mock
echo '{"projects":[]}' > "$MOCK_DIR/projects.json"
cat > "$MOCK_DIR/fields.json" <<'JSON'
{"fields":[{"id":"F_STATUS","name":"Status","type":"ProjectV2SingleSelectField",
 "options":[{"id":"o_todo","name":"Todo"},{"id":"o_progress","name":"In progress"},{"id":"o_done","name":"Done"}]}]}
JSON
cp "$HERE/fields-status-only.json" "$MOCK_DIR/fields-after.json"
run_script
check "exits 0"                    '[[ $RC -eq 0 ]]'
check "creates the project"        'grep -q "project create" "$MOCK_LOG"'
check "creates Area"               'grep -q "field-create 1 --owner Trisert --name Area" "$MOCK_LOG"'
check "creates Priority"           'grep -q "field-create 1 --owner Trisert --name Priority" "$MOCK_LOG"'
check "creates TASKS.md row"       'grep -q "field-create 1 --owner Trisert --name TASKS.md row" "$MOCK_LOG"'
rm -rf "$MOCK_DIR"

# --- T9: option replacement must not lose card assignments ------------------
echo "T9 Status reconciliation restores assignments it cleared"
new_mock
cat > "$MOCK_DIR/fields.json" <<'JSON'
{"fields":[
 {"id":"F_STATUS","name":"Status","type":"ProjectV2SingleSelectField","options":[
   {"id":"o_todo","name":"Todo"},{"id":"o_blocked","name":"Blocked"},{"id":"o_done","name":"Done"}]},
 {"id":"F_AREA","name":"Area","type":"ProjectV2SingleSelectField","options":[
   {"id":"a_firmware","name":"Firmware"},{"id":"a_comms","name":"COMMS"},{"id":"a_eps","name":"EPS"},
   {"id":"a_aocs","name":"AOCS"},{"id":"a_payloads","name":"Payloads"},{"id":"a_ci","name":"CI"},
   {"id":"a_docs","name":"Docs"}]},
 {"id":"F_PRIO","name":"Priority","type":"ProjectV2SingleSelectField","options":[
   {"id":"p_p0","name":"P0"},{"id":"p_p1","name":"P1"},{"id":"p_p2","name":"P2"}]},
 {"id":"F_ROW","name":"TASKS.md row","type":"ProjectV2Field","options":[]}
]}
JSON
cp "$HERE/fields-after-replaced.json" "$MOCK_DIR/fields-after.json"
# A closed card, not reachable from the open-item import loop.
cat > "$MOCK_DIR/items.json" <<'JSON'
{"items":[{"id":"ITEM_CLOSED","content":{"number":1,"url":"https://github.com/Trisert/JOS/issues/1"},"status":"Blocked"}]}
JSON
run_script
check "exits 0"                          '[[ $RC -eq 0 ]]'
check "snapshot precedes the mutation"   '[[ $(grep -n "project item-list" "$MOCK_LOG" | head -1 | cut -d: -f1) -lt $(grep -n "api graphql" "$MOCK_LOG" | head -1 | cut -d: -f1) ]]'
check "restores on a new option id"      'grep -q -- "--id ITEM_CLOSED --project-id PID --field-id F_STATUS --single-select-option-id o2_blocked" "$MOCK_LOG"'
rm -rf "$MOCK_DIR"

# --- T10: an oversized board is refused, not truncated ----------------------
echo "T10 board larger than the snapshot window refuses to reconcile"
new_mock
cat > "$MOCK_DIR/fields.json" <<'JSON'
{"fields":[{"id":"F_STATUS","name":"Status","type":"ProjectV2SingleSelectField","options":[
  {"id":"o_todo","name":"Todo"},{"id":"o_blocked","name":"Blocked"},{"id":"o_done","name":"Done"}]}]}
JSON
echo '{"items":[],"totalCount":1001}' > "$MOCK_DIR/items.json"
run_script
check "exits nonzero"                '[[ $RC -ne 0 ]]'
check "says why"                     'grep -q "refusing to reconcile" <<<"$OUT"'
check "sends no mutation"            '! grep -q "api graphql" "$MOCK_LOG"'
rm -rf "$MOCK_DIR"

# --- T11: a rejected restoration is reported, not swallowed -----------------
echo "T11 rejected restoration exits nonzero and names the card"
new_mock
cat > "$MOCK_DIR/fields.json" <<'JSON'
{"fields":[{"id":"F_STATUS","name":"Status","type":"ProjectV2SingleSelectField","options":[
  {"id":"o_todo","name":"Todo"},{"id":"o_blocked","name":"Blocked"},{"id":"o_done","name":"Done"}]}]}
JSON
cp "$HERE/fields-after-replaced.json" "$MOCK_DIR/fields-after.json"
cat > "$MOCK_DIR/items.json" <<'JSON'
{"items":[{"id":"ITEM_CLOSED","content":{"number":1,"url":"https://github.com/Trisert/JOS/issues/1"},"status":"Blocked"}]}
JSON
: > "$MOCK_DIR/fail-edits"
run_script
check "exits nonzero"                '[[ $RC -ne 0 ]]'
check "names the card"               'grep -q "could not restore Status .Blocked. on item ITEM_CLOSED" <<<"$OUT"'
check "says how many"                'grep -q "Status assignment(s) could not be restored" <<<"$OUT"'
rm -rf "$MOCK_DIR"

rm -rf "$MOCK_DIR"

# --- T12: unreadable labels must not become a default status ----------------
echo "T12 unreadable labels leave the card untouched"
new_mock
echo '[{"number":7,"url":"https://github.com/Trisert/JOS/issues/7"}]' > "$MOCK_DIR/issues.json"
issue 7 open false area/firmware status/blocked
: > "$MOCK_DIR/fail-labels"
run_script
check "exits 0"                  '[[ $RC -eq 0 ]]'
check "warns about the item"     'grep -q "cannot read the labels of #7" <<<"$OUT"'
check "creates no card"          '! grep -q "item-add" "$MOCK_LOG"'
check "writes no field"          '! grep -q "item-edit" "$MOCK_LOG"'
rm -rf "$MOCK_DIR"

# --- T13: a repository bigger than the window is refused --------------------
echo "T13 more open items than the window aborts before any import"
new_mock
echo '{"open_issues_count":1001}' > "$MOCK_DIR/repo.json"
echo '[{"number":7,"url":"https://github.com/Trisert/JOS/issues/7"}]' > "$MOCK_DIR/issues.json"
issue 7 open false area/firmware
run_script
check "exits nonzero"            '[[ $RC -ne 0 ]]'
check "names the window"         'grep -q "exceed the 1000-item window" <<<"$OUT"'
check "imports nothing"          '! grep -q "item-add" "$MOCK_LOG"'
check "edits nothing"            '! grep -q "item-edit" "$MOCK_LOG"'
rm -rf "$MOCK_DIR"

# --- T14: the label table is printed, not applied, in dry-run ---------------
echo "T14 label taxonomy dry run is complete and unique"
PLAN="$(DRY_RUN=1 "$HERE/../sync-labels.sh" 2>/dev/null)"; PRC=$?
check "exits 0"                  '[[ $PRC -eq 0 ]]'
check "15 labels"                '[[ $(grep -c . <<<"$PLAN") -eq 15 ]]'
check "area/docs 0052cc"         'grep -qP "^area/docs\t0052cc\t" <<<"$PLAN"'
check "names unique"             '[[ $(cut -f1 <<<"$PLAN" | sort -u | grep -c .) -eq 15 ]]'
check "colours are 6 hex"        '! cut -f2 <<<"$PLAN" | grep -qvE "^[0-9a-f]{6}$"'
rm -rf "$MOCK_DIR"

# --- T15: applying the taxonomy calls gh once per label with --force --------
echo "T15 applying the taxonomy creates every label idempotently"
new_mock
GH="$FAKE" MOCK_DIR="$MOCK_DIR" MOCK_LOG="$MOCK_LOG" "$HERE/../sync-labels.sh" Trisert/JOS >/dev/null 2>&1; SRC=$?
check "exits 0"                  '[[ $SRC -eq 0 ]]'
check "15 label create calls"    '[[ $(grep -c "label create" "$MOCK_LOG") -eq 15 ]]'
check "all use --force"          '[[ $(grep -c -- "--force" "$MOCK_LOG") -eq 15 ]]'
rm -rf "$MOCK_DIR"

# --- T16: a malformed table is rejected before any API call -----------------
echo "T16 a duplicate label is rejected before touching GitHub"
new_mock
bad="$(mktemp)"; printf 'task|0e8a16|one\ntask|0e8a16|two\n' > "$bad"
GH="$FAKE" MOCK_DIR="$MOCK_DIR" MOCK_LOG="$MOCK_LOG" LABELS_FILE="$bad" \
  "$HERE/../sync-labels.sh" Trisert/JOS >/dev/null 2>"$MOCK_DIR/err"; BRC=$?
check "exits nonzero"            '[[ $BRC -ne 0 ]]'
check "names the duplicate"      'grep -q "duplicate label task" "$MOCK_DIR/err"'
check "sends nothing"            '[[ ! -s "$MOCK_LOG" ]]'
rm -f "$bad"; rm -rf "$MOCK_DIR"

echo
echo "passed: $pass  failed: $fail"
(( fail == 0 ))