#ifndef STATE_MACHINE_H
#define STATE_MACHINE_H

#include "obsw_types.h"
#include "cmsis_os.h"
#include <stddef.h>

/* ---------- Public API ---------- */

/* Initialise the state machine (call once before osKernelStart) */
void state_machine_init(void);

/* Create the state machine FreeRTOS task */
osThreadId_t state_machine_task_create(void);

/* Get the current operational state (thread-safe) */
obw_state_t state_machine_get_state(void);

/* Request a state transition (called from BMS, comms, etc.) */
int state_machine_request_transition(obw_state_t target, uint8_t trigger);

/* Get the beacon interval for the current state */
uint32_t state_machine_get_beacon_interval(void);

/* Set a custom beacon interval override (ms), 0 = use default per-state.
   Non-zero values must lie within [BEACON_INTERVAL_MIN, BEACON_INTERVAL_MAX];
   out-of-range requests are rejected and the current override is left intact.
   Returns 0 on acceptance, -1 on rejection. */
int state_machine_set_beacon_interval(uint32_t interval_ms);

/* ---------- SEU scrubbing hook (W2-5) ----------
   Address and size of the critical OBSW state structure, so seu_mitigation.c
   can keep a redundant copy of it and rewrite it if a bit flips. The struct
   itself stays private to state_machine.c; only its extent is published. */
void *state_machine_critical_region(size_t *len);

#endif /* STATE_MACHINE_H */
