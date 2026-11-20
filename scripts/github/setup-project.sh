#!/usr/bin/env bash
# JOS project board - idempotent bootstrap.
#
# Creates the `JOS` project if missing, reconciles the custom fields and the
# Status options, imports every open issue and pull request, and derives
# Area / Priority / Status from labels. Safe to re-run at any time.
#
# Requires the `project` scope (gh auth refresh -s project).
#
# Not derived, set by hand once per card: the "TASKS.md row" text field.
#   gh project item-edit --id <item> --project-id <pid> \
#     --field-id <TASKS field id> --text 49
#
# Set GH to another executable to run the host tests (scripts/github/test/).
set -euo pipefail

GH="${GH:-gh}"
OWNER="${OWNER:-Trisert}"
REPO="${REPO:-Trisert/JOS}"
TITLE="${TITLE:-JOS}"

command -v jq >/dev/null || { echo "jq is required" >&2; exit 1; }

# The five states the board must offer; the built-in field ships three.
REQUIRED_STATUS_JSON='[
  {"name":"Todo","color":"GRAY","description":"Defined, not started"},
  {"name":"In progress","color":"YELLOW","description":"Branch or worktree open"},
  {"name":"In review","color":"PURPLE","description":"PR open, awaiting review or approval"},
  {"name":"Blocked","color":"RED","description":"Waiting on hardware, a document or a decision"},
  {"name":"Done","color":"GREEN","description":"Merged or closed"}
]'

echo "== project"
project="$("$GH" project list --owner "$OWNER" --format json --limit 100 \
  | jq -r --arg t "$TITLE" '.projects[] | select(.title==$t) | .number' | head -1)"
if [[ -z "$project" ]]; then
  project="$("$GH" project create --owner "$OWNER" --title "$TITLE" --format json | jq -r .number)"
  echo "   created #$project"
else
  echo "   found #$project"
fi
pid="$("$GH" project list --owner "$OWNER" --format json --limit 100 \
  | jq -r --arg n "$project" '.projects[] | select((.number|tostring)==$n) | .id')"

echo "== fields"
fields="$("$GH" project field-list "$project" --owner "$OWNER" --format json)"
fid() { jq -r --arg n "$1" '.fields[] | select(.name==$n) | .id' <<<"$fields"; }
oid() { jq -r --arg f "$1" --arg o "$2" '.fields[] | select(.name==$f) | .options[]? | select(.name==$o) | .id' <<<"$fields"; }
have() { [[ -n "$(fid "$1")" && "$(fid "$1")" != "null" ]]; }

# Reconcile Status: add every required option that is missing and keep any
# option already on the field. Checking one option is not enough - a project
# that already has "Blocked" can still lack "In review".
status_field="$(fid Status)"
if [[ -z "$status_field" || "$status_field" == "null" ]]; then
  echo "   ! project has no Status field to reconcile" >&2
  exit 1
fi
missing="$("$GH" project field-list "$project" --owner "$OWNER" --format json \
  | jq -r --argjson req "$REQUIRED_STATUS_JSON" '
      [.fields[] | select(.name=="Status") | .options[]?.name] as $have
      | $req[] | select(.name as $n | ($have | index($n)) | not) | .name')"
if [[ -n "$missing" ]]; then
  echo "   adding Status options: $(tr '\n' ' ' <<<"$missing")"
  # Replacing the option list recreates every option's identity and clears the
  # Status of every card, including closed items, drafts and cards from other
  # repositories. Snapshot first, restore after: the import loop only knows
  # about open items in this repository.
  snapshot="$("$GH" project item-list "$project" --owner "$OWNER" --format json --limit 1000)"
  snapshot_total="$(jq -r '.totalCount // (.items | length)' <<<"$snapshot")"
  if (( snapshot_total > 1000 )); then
    echo "   ! ${snapshot_total} items on the board: the snapshot would be incomplete, refusing to reconcile" >&2
    exit 1
  fi
  options="$("$GH" project field-list "$project" --owner "$OWNER" --format json \
    | jq -c --argjson req "$REQUIRED_STATUS_JSON" '
        [.fields[] | select(.name=="Status") | .options[]? | {name, color:"GRAY", description:""}] as $cur
        | $req + [ $cur[] | select(.name as $n | ($req | map(.name) | index($n)) | not) ]')"
  jq -n --arg fid "$status_field" --argjson opts "$options" \
    '{query: "mutation($fieldId: ID!, $options: [ProjectV2SingleSelectFieldOptionInput!]!) { updateProjectV2Field(input: {fieldId: $fieldId, singleSelectOptions: $options}) { projectV2Field { ... on ProjectV2SingleSelectField { name } } } }",
      variables: {fieldId: $fid, options: $opts}}' \
    | "$GH" api graphql --input - >/dev/null
  fields="$("$GH" project field-list "$project" --owner "$OWNER" --format json)"

  restore_failed=0
  while IFS=$'\t' read -r item saved; do
    [[ -z "$item" || -z "$saved" ]] && continue
    new_oid="$(oid Status "$saved")"
    if [[ -z "$new_oid" ]] || ! "$GH" project item-edit --id "$item" --project-id "$pid" \
          --field-id "$status_field" --single-select-option-id "$new_oid" >/dev/null 2>&1; then
      echo "   ! could not restore Status '${saved}' on item ${item}" >&2
      restore_failed=$((restore_failed + 1))
    fi
  done < <(jq -r '.items[] | select(.status != null and .status != "") | [.id, .status] | @tsv' <<<"$snapshot")
  if (( restore_failed > 0 )); then
    echo "   ! ${restore_failed} Status assignment(s) could not be restored; re-set them by hand" >&2
    exit 1
  fi
fi

have Area || { echo "   creating Area"; "$GH" project field-create "$project" --owner "$OWNER" --name Area \
  --data-type SINGLE_SELECT --single-select-options "Firmware,COMMS,EPS,AOCS,Payloads,CI,Docs" >/dev/null; }
have Priority || { echo "   creating Priority"; "$GH" project field-create "$project" --owner "$OWNER" --name Priority \
  --data-type SINGLE_SELECT --single-select-options "P0,P1,P2" >/dev/null; }
have "TASKS.md row" || { echo "   creating TASKS.md row"; "$GH" project field-create "$project" --owner "$OWNER" \
  --name "TASKS.md row" --data-type TEXT >/dev/null; }

# Reload after creation.
fields="$("$GH" project field-list "$project" --owner "$OWNER" --format json)"
area_f="$(fid Area)"; prio_f="$(fid Priority)"

area_opt() {
  case "$1" in
    area/firmware) echo Firmware ;;
    area/comms)    echo COMMS ;;
    area/eps)      echo EPS ;;
    area/aocs)     echo AOCS ;;
    area/payloads) echo Payloads ;;
    area/ci)       echo CI ;;
    area/docs)     echo Docs ;;
    *)             echo "" ;;
  esac
}

# A rejected field update must not read as success.
edits_failed=0
set_field() {
  local item="$1" field="$2" kind="$3" value="$4" out
  if [[ -z "$field" || -z "$value" ]]; then
    echo "   ! cannot set ${kind} on item ${item}: missing field id or value" >&2
    edits_failed=$((edits_failed + 1))
    return 0
  fi
  if ! out="$("$GH" project item-edit --id "$item" --project-id "$pid" \
        --field-id "$field" "$kind" "$value" 2>&1)"; then
    echo "   ! field update failed on item ${item}: ${out:-no diagnostics}" >&2
    edits_failed=$((edits_failed + 1))
  fi
  return 0
}

echo "== import open items"
items="$("$GH" project item-list "$project" --owner "$OWNER" --format json --limit 200)"
prs="$("$GH" pr list --repo "$REPO" --state open --limit 100 --json number,body)"
urls="$("$GH" pr list --repo "$REPO" --state open --limit 100 --json url --jq '.[].url'
        "$GH" issue list --repo "$REPO" --state open --limit 100 --json url --jq '.[].url')"

# Issues an open PR closes are in review, not todo: `Closes #N` is the contract.
# The search legitimately finds nothing when there are no PRs or no references;
# under `set -e`/`pipefail` that must not abort the import.
in_review="$( { jq -r '.[].body // ""' <<<"$prs" \
  | grep -oiE '(close[sd]?|fix(e[sd])?|resolve[sd]?) +#[0-9]+' \
  | grep -oE '[0-9]+' | sort -u; } || true )"

for url in $urls; do
  n="$(basename "$url")"
  item="$(jq -r --arg u "$url" '.items[] | select(.content.url==$u) | .id' <<<"$items" | head -1)"
  if [[ -z "$item" ]]; then
    item="$("$GH" project item-add "$project" --owner "$OWNER" --url "$url" --format json | jq -r .id)"
    echo "   + $url"
  fi

  labels="$("$GH" api "repos/${REPO}/issues/${n}/labels" --jq '.[].name' 2>/dev/null || true)"
  meta="$("$GH" api "repos/${REPO}/issues/${n}" \
    --jq '[.state, (has("pull_request")|tostring)] | join(" ")' 2>/dev/null || true)"
  # An unreadable item is not a Todo: leave the card alone and say so.
  if [[ "$meta" != "open "* && "$meta" != "closed "* ]]; then
    echo "   ! cannot read the state of #${n} (got '${meta}'); fields left untouched" >&2
    continue
  fi
  state="${meta%% *}"; is_pr="${meta##* }"

  area=""; prio=""; status="Todo"
  for l in $labels; do
    [[ -z "$area" ]] && area="$(area_opt "$l")"
    case "$l" in
      prio/P0) prio=P0 ;;
      prio/P1) prio=P1 ;;
      prio/P2) prio=P2 ;;
      status/blocked) status=Blocked ;;
      status/needs-sys-ele) status=Blocked ;;
    esac
  done
  [[ "$state" == "closed" ]] && status=Done
  [[ "$is_pr" == "true" && "$state" == "open" ]] && status="In review"
  if [[ "$is_pr" == "false" && "$state" == "open" ]] && grep -qx "$n" <<<"$in_review"; then
    status="In review"
  fi

  # Only set a field when a value is known: never overwrite a hand-set cell
  # with an empty default.
  if [[ -n "$area" ]]; then
    set_field "$item" "$area_f" --single-select-option-id "$(oid Area "$area")"
  fi
  if [[ -n "$prio" ]]; then
    set_field "$item" "$prio_f" --single-select-option-id "$(oid Priority "$prio")"
  fi
  set_field "$item" "$status_field" --single-select-option-id "$(oid Status "$status")"
done

echo "== done: https://github.com/users/$OWNER/projects/$project"
if (( edits_failed > 0 )); then
  echo "ERROR: ${edits_failed} field update(s) failed" >&2
  exit 1
fi
