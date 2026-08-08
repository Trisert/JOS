/**
  ******************************************************************************
  * @file    support/seu_stubs.c
  * @brief   Host definitions for the SEU-mitigation entry points that
  *          App/memory/memory.c calls (W2-5).
  *
  * The scrubber (Core/Src/seu_mitigation.c) is target-only: it needs the RTOS,
  * PRIMASK and the RTC backup domain. On the host the three entry points used
  * by the LastStates ring are reduced to bookkeeping, so the flight code path
  * stays unmodified and a test can still assert that every mirror update is
  * wrapped in lock/unlock and followed by a commit (the ownership contract
  * documented in Core/Inc/seu_mitigation.h).
  *
  * Linked into every test executable (Ceedling links all :support: files), so
  * it must not reference symbols from a specific module.
  ******************************************************************************
  */

#include "seu_mitigation.h"

static int lock_depth        = 0;
static int commit_count      = 0;
static int last_commit_regid = -1;

void seu_mitigation_lock(void)
{
    lock_depth++;
}

void seu_mitigation_unlock(void)
{
    if (lock_depth > 0) {
        lock_depth--;
    }
}

int seu_mitigation_commit(seu_region_id_t id)
{
    if ((int)id < 0 || (int)id >= (int)SEU_REGION_ID_COUNT) {
        return -1;
    }
    commit_count++;
    last_commit_regid = (int)id;
    return 0;
}

int seu_stub_lock_depth(void)         { return lock_depth; }
int seu_stub_commit_count(void)       { return commit_count; }
int seu_stub_last_commit_region(void) { return last_commit_regid; }

void seu_stub_reset(void)
{
    lock_depth        = 0;
    commit_count      = 0;
    last_commit_regid = -1;
}
