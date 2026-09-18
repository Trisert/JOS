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
The CodeRabbit follow-up adds stateful CMSIS flags and EXTI/NVIC doubles:
timeout-racing and late teardown IRQs, stale pending events at next start,
immediate new TX/RX completion, failed quiesce and IRQ-enable restoration.
Failed TX start now asserts both finishTransmit cleanup and RX recovery.

The driver masks only EXTI0 across transitions (DMA and TIM6 stay enabled).
It stops the old radio source before clearing its IRQ state via finishReceive,
drains EXTI/NVIC pending state before arming new work, clears stale TX flags,
and publishes explicit Idle/TX/RX routing. New completion events are not
cleared after arming. Failed quiesce leaves routing Idle and reports failure.

Five mutation checks failed at their intended assertions: removed start-failure
cleanup, omitted pending-event drain, cleared a new completion, omitted flag
clearing, and incorrect IRQ-enable restoration. The unmutated control passes.
These are deterministic interleaving tests, not RF or scheduler verification.

Full local gates: `make release`, `make crc-stamp`, `make test`, `make coverage`,
`make cppcheck`, `make cppcheck-canary`, `make cppcheck-includes`,
`make cppcheck-includes-proof`; repository-root `actionlint`.

## Remaining boundaries

No EPS/AOCS protocol, TT&C MAC scheme, payload scheduler or telemetry encoder
was invented. Their documented integration blockers remain. No pin assignments,
vendored libraries, authentication enforcement or coverage thresholds changed.
DIO1 transition interleavings are now covered by host tests. Shared TX/RX task
and SPI serialization still need HIL/concurrency review: EXTI masking is not a
mutex and does not solve the entire radio ownership architecture. A failed rearm
is retried and does not feed watchdog liveness until RX is armed; a permanently
failed radio remains fail-closed for watchdog recovery.

The initial R1–R6 implementation was self-reviewed. CodeRabbit reviewed commit
73473e7 and reported two radio findings addressed in this follow-up. An in-house
review attempt was interrupted by an API error; it provided no verdict.
This verification is not independent approval or flight qualification. Do not
merge solely because host CI is green.
