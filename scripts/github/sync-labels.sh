#!/usr/bin/env bash
# JOS label taxonomy - idempotent.
#
# Usage:
#   ./scripts/github/sync-labels.sh [owner/repo]     apply the taxonomy
#   DRY_RUN=1 ./scripts/github/sync-labels.sh        print the plan, no API call
#
# Requires a token with the `repo` scope (gh auth status). Re-running is safe:
# `gh label create --force` updates colour and description.
#
# The table is validated before any API call: a duplicate name, a malformed
# colour or a description over 100 characters aborts the run instead of
# creating half a taxonomy.
set -euo pipefail

GH="${GH:-gh}"
REPO="${1:-${REPO:-Trisert/JOS}}"
DRY_RUN="${DRY_RUN:-0}"
LABELS_FILE="${LABELS_FILE:-}"

# name|colour|description  (flat, because GitHub labels have no hierarchy)
read -r -d '' TABLE <<'TABLE' || true
task|0e8a16|Planned work tracked by a TASKS.md row
decision|5319e7|Needs an answer from SYS, ELE or the ground segment
hil|fbca04|Cannot be closed without hardware in the loop
area/firmware|1d76db|App/ and hand-written Core/ sources
area/comms|c2e0c6|TT&C: RadioLib/SX1268, beacon, frames, uplink auth
area/eps|b60205|EPS interface: BMS, SoC gates, SPI slave link
area/aocs|006b75|AOCS interface and telemetry contract
area/payloads|a2eeef|CRYSTALS / CLOUD / CLEAR payload paths and scheduler
area/ci|6e7781|Workflows, toolchains and gates
area/docs|0052cc|Documentation-only work with no single subsystem
prio/P0|b60205|Wrong flight behaviour, or a silently dead gate
prio/P1|d93f0b|Wrong result in a non-flight path, or a qualification blocker
prio/P2|fbca04|Opportunistic work, documentation polish
status/blocked|e11d21|Waiting on a dependency: hardware, document or decision
status/needs-sys-ele|d4c5f9|Awaiting confirmation from the SYS or ELE subteam
TABLE

# Test seam: an external table lets the tests feed a malformed taxonomy.
[[ -n "$LABELS_FILE" ]] && TABLE="$(cat "$LABELS_FILE")"

validate() {
  awk -F'|' '
    /^[[:space:]]*$/ { next }
    {
      if (NF != 3) { printf "line %d: expected name|color|description\n", NR > "/dev/stderr"; bad = 1; next }
      if ($1 !~ /^[A-Za-z0-9][A-Za-z0-9._\/-]*$/) { printf "line %d: bad name %s\n", NR, $1 > "/dev/stderr"; bad = 1 }
      if ($2 !~ /^[0-9a-f]{6}$/) { printf "line %d: bad colour %s (want 6 lowercase hex)\n", NR, $2 > "/dev/stderr"; bad = 1 }
      if (length($3) == 0 || length($3) > 100) { printf "line %d: description must be 1-100 characters\n", NR > "/dev/stderr"; bad = 1 }
      if (seen[$1]++) { printf "line %d: duplicate label %s\n", NR, $1 > "/dev/stderr"; bad = 1 }
      total++
    }
    END {
      if (total < 1) { print "empty label table" > "/dev/stderr"; bad = 1 }
      exit bad
    }
  ' <<<"$TABLE"
}

if ! validate; then
  echo "label table rejected: nothing was sent to GitHub" >&2
  exit 1
fi

count="$(printf '%s\n' "$TABLE" | grep -c . || true)"

if [[ "$DRY_RUN" == "1" ]]; then
  printf '%s\n' "$TABLE" | grep . | awk -F'|' '{ printf "%s\t%s\t%s\n", $1, $2, $3 }'
  echo "# ${count} labels, dry run: nothing sent to GitHub" >&2
  exit 0
fi

echo "Syncing ${count} labels on $REPO"
printf '%s\n' "$TABLE" | grep . | while IFS='|' read -r name colour desc; do
  "$GH" label create "$name" --color "$colour" --description "$desc" --repo "$REPO" --force >/dev/null
  printf '  %-24s %s\n' "$name" "$desc"
done

echo "Done. Legacy catch-all 'jos' is intentionally not recreated."
