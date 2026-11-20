#!/usr/bin/env bash
# JOS project board - idempotent bootstrap.
#
# Creates the `JOS` project if missing, ensures the custom fields and Status
# options exist, imports every open issue and pull request, and derives
# Area / Priority / Status from labels. Safe to re-run at any time.
#
# Requires the `project` scope (gh auth refresh -s project).
#
# Not derived, set by hand once per card: the "TASKS.md row" text field.
# Example: gh project item-edit --id <item> --project-id <pid> \
#            --field-id <TASKS field id> --text 49
set -euo pipefail

OWNER="${OWNER:-Trisert}"
REPO="${REPO:-Trisert/JOS}"
TITLE="${TITLE:-JOS}"

command -v jq >/dev/null || { echo "jq is required" >&2; exit 1; }

echo "== project"
project="$(gh project list --owner "$OWNER" --format json --limit 100 \
  | jq -r --arg t "$TITLE" '.projects[] | select(.title==$t) | .number' | head -1)"
if [[ -z "$project" ]]; then
  project="$(gh project create --owner "$OWNER" --title "$TITLE" --format json | jq -r .number)"
  echo "   created #$project"
else
  echo "   found #$project"
fi
pid="$(gh project list --owner "$OWNER" --format json --limit 100 \
  | jq -r --arg n "$project" '.projects[] | select((.number|tostring)==$n) | .id')"

echo "== fields"
fields="$(gh project field-list "$project" --owner "$OWNER" --format json)"
fid() { jq -r --arg n "$1" '.fields[] | select(.name==$n) | .id' <<<"$fields"; }
oid() { jq -r --arg f "$1" --arg o "$2" '.fields[] | select(.name==$f) | .options[]? | select(.name==$o) | .id' <<<"$fields"; }

have() { [[ -n "$(fid "$1")" && "$(fid "$1")" != "null" ]]; }

# Status options, including the two the built-in field does not ship with.
status_field="$(fid Status)"
if [[ -z "$(oid Status 'Blocked')" ]]; then
  echo "   adding Status options"
  gh api graphql -f fieldId="$status_field" -f query='
    mutation($fieldId: ID!) {
      updateProjectV2Field(input: {
        fieldId: $fieldId
        singleSelectOptions: [
          {name: "Todo", color: GRAY, description: "Defined, not started"}
          {name: "In progress", color: YELLOW, description: "Branch or worktree open"}
          {name: "In review", color: PURPLE, description: "PR open, awaiting review or approval"}
          {name: "Blocked", color: RED, description: "Waiting on hardware, a document or a decision"}
          {name: "Done", color: GREEN, description: "Merged or closed"}
        ]
      }) { projectV2Field { ... on ProjectV2SingleSelectField { name } } }
    }' >/dev/null
fi

have Area || { echo "   creating Area"; gh project field-create "$project" --owner "$OWNER" --name Area \
  --data-type SINGLE_SELECT --single-select-options "Firmware,COMMS,EPS,AOCS,Payloads,CI,Docs" >/dev/null; }
have Priority || { echo "   creating Priority"; gh project field-create "$project" --owner "$OWNER" --name Priority \
  --data-type SINGLE_SELECT --single-select-options "P0,P1,P2" >/dev/null; }
have "TASKS.md row" || { echo "   creating TASKS.md row"; gh project field-create "$project" --owner "$OWNER" \
  --name "TASKS.md row" --data-type TEXT >/dev/null; }

# Reload after creation.
fields="$(gh project field-list "$project" --owner "$OWNER" --format json)"
area_f="$(fid Area)"; prio_f="$(fid Priority)"

area_opt() {
  case "$1" in
    area/firmware) echo Firmware ;;
    area/comms)    echo COMMS ;;
    area/eps)      echo EPS ;;
    area/aocs)     echo AOCS ;;
    area/payloads) echo Payloads ;;
    area/ci)       echo CI ;;
    *)             echo "" ;;
  esac
}

echo "== import open items"
items="$(gh project item-list "$project" --owner "$OWNER" --format json --limit 200)"
prs="$(gh pr list --repo "$REPO" --state open --limit 100 --json number,body)"
urls="$(gh pr list --repo "$REPO" --state open --limit 100 --json url --jq '.[].url'
        gh issue list --repo "$REPO" --state open --limit 100 --json url --jq '.[].url')"

# Issues an open PR closes are in review, not todo: `Closes #N` is the contract.
in_review="$(jq -r '.[].body // ""' <<<"$prs" \
  | grep -oiE '(close[sd]?|fix(e[sd])?|resolve[sd]?) +#[0-9]+' \
  | grep -oE '[0-9]+' | sort -u)"

for url in $urls; do
  item="$(jq -r --arg u "$url" '.items[] | select(.content.url==$u) | .id' <<<"$items" | head -1)"
  if [[ -z "$item" ]]; then
    item="$(gh project item-add "$project" --owner "$OWNER" --url "$url" --format json | jq -r .id)"
    echo "   + $url"
  fi

  labels="$(gh api "repos/${REPO}/issues/$(basename "$url")/labels" --jq '.[].name' 2>/dev/null || true)"
  state="$(gh api "repos/${REPO}/issues/$(basename "$url")" --jq '.state' 2>/dev/null || echo open)"
  is_pr="$(gh api "repos/${REPO}/issues/$(basename "$url")" --jq 'has("pull_request")' 2>/dev/null || echo false)"

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
  n="$(basename "$url")"
  if [[ "$is_pr" == "false" && "$state" == "open" ]] && grep -qx "$n" <<<"$in_review"; then
    status="In review"
  fi

  # Only set a field when a value is known: never overwrite a hand-set cell
  # with an empty default.
  if [[ -n "$area" ]]; then
    gh project item-edit --id "$item" --project-id "$pid" --field-id "$area_f" \
      --single-select-option-id "$(oid Area "$area")" >/dev/null 2>&1 || true
  fi
  if [[ -n "$prio" ]]; then
    gh project item-edit --id "$item" --project-id "$pid" --field-id "$prio_f" \
      --single-select-option-id "$(oid Priority "$prio")" >/dev/null 2>&1 || true
  fi
  gh project item-edit --id "$item" --project-id "$pid" --field-id "$status_field" \
    --single-select-option-id "$(oid Status "$status")" >/dev/null 2>&1 || true
done

echo "== done: https://github.com/users/$OWNER/projects/$project"
