# Direct-review safety corrections

Scope: findings R1–R6 from the direct review of `7f0aabd`.

- R1: READY→ACTIVE uses the same valid SoC >= B_OPOK gate as CRIT→ACTIVE.
- R2: the autonomous low-battery response uses unconditional CRIT entry. Failed
  Flash logging no longer prevents containment; the record remains best effort.
- R3: ongoing low battery while already CRIT does not log or sync another
  self-transition. Explicit fault records are not globally suppressed.
- R4: asynchronous TX completion and failure attempt finishTransmit followed by
  continuous RX rearm. Completion returns failure on TX timeout, finish failure
  or RX rearm failure. TX start failure attempts cleanup and rearm too.
- R5: after FRAM restoration, with SEU commits enabled and before task creation,
  normalize nominal operating state to OFF and invalidate old battery telemetry.
  Preserve CRIT. The new live state is committed to the shadow/CRC pair so the
  scrubber cannot undo normalization. FRAM is not rewritten by this boot hook.
- R6: the old opcode dispatcher propagates owner refusal and explicitly rejects
  SET_CONFIG/SEND_DATA as unsupported. It does NOT implement those commands.
  EXECUTION and UNSUPPORTED verdicts are appended, preserving existing numeric
  values. They increment aggregate rejections, not malformed counts. RESET is
  accounted before its non-returning reset; other commands after execution.

## Verification

Existing host suite plus seven new tests: 411 tests. The state-machine tests
exercise validity, the 79/80 threshold boundary, failed Flash containment,
repeated low-battery loops and the post-restore normalization API. SEU itself
is target-only in this suite: full physical restore/first-scrub behavior still
needs target/HIL verification, not merely the no-op SEU host double.

`make -C JOS/test native` builds the actual production radio driver with strict
local HAL/RadioLib doubles. It covers TX success, timeout, start failure, RX
rearm failure and finish failure. Wired into `make test` and the host CI job.
It verifies lifecycle calls, not RF behavior or scheduler concurrency.

Full local gates: `make release`, `make crc-stamp`, `make test`, `make coverage`,
`make cppcheck`, `make cppcheck-canary`, `make cppcheck-includes`,
`make cppcheck-includes-proof`; repository-root `actionlint`.

## Remaining boundaries

No EPS/AOCS protocol, TT&C MAC scheme, payload scheduler or telemetry encoder
was invented. Their documented integration blockers remain. No pin assignments,
vendored libraries, authentication enforcement or coverage thresholds changed.
The existing radio ISR's mode heuristic and shared TX/RX concurrency still need
HIL/concurrency review; this patch repairs the missing TX→RX lifecycle, not the
entire radio ownership architecture. A failed rearm is reported as failure;
there is no new automatic recovery policy for a permanently failed radio.

No subagent review was used, following the direct-work request. This is an
implementation/self-review handoff, not an independent approval or flight
qualification. Do not merge solely because host CI is green.
