/**
  ******************************************************************************
  * @file    fakes/seu_mitigation.h
  * @brief   Host stand-in for Core/Inc/seu_mitigation.h (W2-5).
  *
  * The real header lives in Core/Inc, which is deliberately NOT on the host
  * test include path (it also holds the CubeMX main.h, which would shadow
  * test/fakes/main.h and drag the whole HAL/CMSIS tree into the host build),
  * and it includes cmsis_os2.h for osThreadId_t. The scrubber itself is
  * target-only; App/memory/memory.c needs exactly the three entry points and
  * the one region id declared below, so the host build gets this narrow fake
  * instead of an #ifdef around every call site in the flight code.
  *
  * Signatures MUST stay byte-compatible with Core/Inc/seu_mitigation.h; the
  * region ids mirror seu_region_id_t there. Definitions of the three
  * functions live in test/support/seu_stubs.c.
  ******************************************************************************
  */

#ifndef SEU_MITIGATION_H
#define SEU_MITIGATION_H

#include <stdint.h>
#include <stddef.h>

/* Mirrors seu_region_id_t in Core/Inc/seu_mitigation.h (same values). */
typedef enum {
    SEU_REGION_OBSW_STATE   = 0,
    SEU_REGION_LASTSTATES   = 1,
    SEU_REGION_COMMS_BEACON = 2,
    SEU_REGION_COMMS_RX     = 3,
    SEU_REGION_COMMS_TX     = 4,
    SEU_REGION_ID_COUNT     = 5,
} seu_region_id_t;

/** Interrupt-safe mutual exclusion against the scrub task (PRIMASK on the
 *  target; a nesting counter on the host, so a test can assert the pairing). */
void seu_mitigation_lock(void);
void seu_mitigation_unlock(void);

/** Re-take the snapshot of a region after a legitimate update.
 *  0 on success, -1 if the region is unknown / not initialised. */
int seu_mitigation_commit(seu_region_id_t id);

/* ---------- host-only introspection (not present on the target) ---------- */

/** Current lock nesting depth; 0 outside a critical section. */
int  seu_stub_lock_depth(void);
/** Number of seu_mitigation_commit() calls since the last reset. */
int  seu_stub_commit_count(void);
/** Region id passed to the most recent commit, -1 if there was none. */
int  seu_stub_last_commit_region(void);
/** Clear the counters above (call from setUp()). */
void seu_stub_reset(void);

#endif /* SEU_MITIGATION_H */
