#!/usr/bin/env bash
# JOS label taxonomy - idempotent.
#
# Usage: ./scripts/github/sync-labels.sh [owner/repo]
# Requires a token with the `repo` scope (gh auth status).
# Re-running is safe: `gh label create --force` updates colour and description.
set -euo pipefail

REPO="${1:-Trisert/JOS}"

label() {
  gh label create "$1" --color "$2" --description "$3" --repo "$REPO" --force >/dev/null
  printf '  %-24s %s\n' "$1" "$3"
}

echo "Syncing labels on $REPO"

# Kind
label "task"     "0e8a16" "Planned work tracked by a TASKS.md row"
label "decision" "5319e7" "Needs an answer from SYS, ELE or the ground segment"
label "hil"      "fbca04" "Cannot be closed without hardware in the loop"

# Area
label "area/firmware" "1d76db" "App/ and hand-written Core/ sources"
label "area/comms"    "c2e0c6" "TT&C: RadioLib/SX1268, beacon, frames, uplink auth"
label "area/eps"      "b60205" "EPS interface: BMS, SoC gates, SPI slave link"
label "area/aocs"     "006b75" "AOCS interface and telemetry contract"
label "area/payloads" "a2eeef" "CRYSTALS / CLOUD / CLEAR payload paths and scheduler"
label "area/ci"       "6e7781" "Workflows, toolchains and gates"
label "area/docs"     "0052cc" "Documentation-only work with no single subsystem"

# Priority
label "prio/P0" "b60205" "Wrong flight behaviour, or a silently dead gate"
label "prio/P1" "d93f0b" "Wrong result in a non-flight path, or a qualification blocker"
label "prio/P2" "fbca04" "Opportunistic work, documentation polish"

# Status
label "status/blocked"      "e11d21" "Waiting on a dependency: hardware, document or decision"
label "status/needs-sys-ele" "d4c5f9" "Awaiting confirmation from the SYS or ELE subteam"

echo "Done. Legacy catch-all 'jos' is intentionally not recreated."
