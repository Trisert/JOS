#ifndef MEMORY_H
#define MEMORY_H

#include "obsw_types.h"
#include <stdint.h>
#include <stddef.h>

/* ---------- FRAM driver ---------- */
void fram_init(void);
int  fram_read(uint32_t addr, uint8_t *buf, size_t len);
int  fram_write(uint32_t addr, const uint8_t *buf, size_t len);

/* ---------- Cyclic buffer ---------- */
void cyclic_buffer_init(void);
int  cyclic_buffer_write(const uint8_t *data, size_t len);
int  cyclic_buffer_read(uint32_t offset, uint8_t *buf, size_t len);
uint32_t cyclic_buffer_head(void);

/* ---------- LastStates pool (internal Flash) ---------- */
void     laststates_init(void);
int      laststates_write(const laststates_entry_t *entry);

/* Dump every COMPLETE record in the pool into `out`.
 *
 * `len` is an in/out parameter and carries the buffer SIZE - the function has
 * no other way to know how much of `out` it may touch:
 *   in : capacity of `out` in bytes (0 is legal and means "tell me the size").
 *   out: on success, the number of bytes written (0 .. 8192);
 *        on failure with -1 and a non-NULL `len`, the number of bytes the
 *        caller must provide, so a retry can size the buffer exactly.
 *
 * A buffer smaller than the required size is REFUSED (-1) and nothing is
 * copied - a full pool is 64 x 128 B = 8 KB, which silently overrunning a
 * caller stack frame would be the classic buffer-overflow defect
 * (NASA-STD-8739.8). Records torn by a reset mid-write are skipped, not
 * reported.
 *
 * Returns 0 on success, -1 on a NULL argument or an undersized buffer. */
int      laststates_dump_all(uint8_t *out, size_t *len);

/* Number of complete records currently held in the pool (0 .. 64). Rescans
   Flash, so it is also the required capacity of laststates_dump_all() divided
   by LASTSTATES_ENTRY_SIZE. */
uint32_t laststates_count(void);

/* ---------- LastStates pool lock (W2-2 review, CRITICAL) ----------
   The pool has TWO independent writers that each run the full
   "pick the first erased slot -> HAL_FLASH_Unlock() -> program 16
   double-words -> HAL_FLASH_Lock()" sequence:

     - laststates_write() above, from stateMachine (osPriorityAboveNormal)
       and loraRX;
     - dual_bank.c:ls_append(), from the watchdog monitor task
       (osPriorityHigh) once it declares the boot successful.

   configUSE_PREEMPTION is 1, so with no serialisation the high-priority
   writer can preempt the other one and either claim the same "first erased"
   slot (-> PROGERR) or slam HAL_FLASH_Lock() shut between two of its
   double-words (-> PGSERR and a permanently torn 128-byte entry).

   One mutex therefore owns the whole select-slot/erase/program sequence in
   BOTH writers, and each writer re-checks that its target slot is still
   erased while holding it.

   laststates_pool_lock() has THREE outcomes, not two (Kilo #21, comment id
   3740842366: "one return value, two completely different meanings"). A lock
   that fails open is a lock-shaped decoration, so "no lock was needed" and
   "the lock could not be taken" must be distinguishable by the caller:

     LASTSTATES_LOCK_HELD (1)
       The mutex was acquired. It MUST be handed back to
       laststates_pool_unlock() so the release can never touch a mutex this
       caller does not own.

     LASTSTATES_LOCK_NOT_NEEDED (0)
       No serialisation is required and none was taken: before
       the scheduler runs (boot is single-threaded: main() calls
       laststates_init() long before osKernelInitialize(), and the boot
       forensics written in that window need no mutex - the kernel state is
       therefore checked BEFORE the degraded latch), or on the exception path
       once the Flash controller has been proven idle and sanitised (see
       below).

     LASTSTATES_LOCK_FAILED (-1)
       Serialisation is REQUIRED but unavailable: osMutexNew() returned NULL
       at init (FreeRTOS heap exhausted), osMutexAcquire() failed, the
       scheduler is running and no mutex exists at all (laststates_init()
       never ran, so both writers can be scheduled against each other), or the
       exception path found the Flash controller mid-sequence and could not
       bring it to a safe state inside its bounded wait. Every writer must
       REFUSE the write and touch no Flash at all — a torn 128-byte entry
       destroys existing forensic history, which is strictly worse than one
       lost record. The refusal is counted (laststates_dropped_records()) so
       the loss is visible from the ground instead of silent. READERS
       (laststates_dump_all()) are exempt: they program nothing, so they take
       the lock for a consistent two-pass scan but continue without it rather
       than deny ground the trail, and they count no dropped record.

   Exception context (the fault, MPU and parity handlers log through
   laststates_write() and then reset) can never block on the mutex. It gets a
   cycle-bounded FLASH_SR.BSY wait plus a FLASH->CR sanitisation instead
   (clear PG/FSTPG/PER/MER1/MER2/PNB and the error flags), because a preempted
   task can leave PG or PER+PNB set with BSY not yet asserted: programming on
   top of that is a programming-sequence error, i.e. exactly the post-mortem
   record we most wanted, filed under "dropped". The preempted task never
   resumes (the handler resets), so taking the controller over is safe. */
#define LASTSTATES_LOCK_FAILED      (-1)
#define LASTSTATES_LOCK_NOT_NEEDED  (0)
#define LASTSTATES_LOCK_HELD        (1)

int      laststates_pool_lock(void);
void     laststates_pool_unlock(int held);

/* Telemetry for the degraded paths above (both saturate-free 32-bit counters,
   zero after reset):
     laststates_lock_failures()   times serialisation was required and could
                                  not be obtained;
     laststates_dropped_records() records refused because of that, i.e. the
                                  forensic entries ground will never see.
   Ground reads these so an unsynchronised pool is observable instead of being
   inferred from missing records. */
uint32_t laststates_lock_failures(void);
uint32_t laststates_dropped_records(void);

/* Bump laststates_dropped_records() from a writer that does NOT go through
   laststates_write(). Core/Src/dual_bank.c:ls_append() programs the pool
   itself, so its boot-fault / boot-OK refusals have to be counted here or the
   tri-state lock stays invisible from the ground. Call it exactly once, on the
   LASTSTATES_LOCK_FAILED path, before returning without touching Flash. */
void     laststates_note_dropped_record(void);

/* ---------- LastStates bookkeeping mirror (SEU scrubbing, W2-5) ----------
   The write index and the entry count decide where the next post-mortem
   record lands: a bit flip there silently overwrites history, or skips the
   page erase and turns the next write into a Flash programming error. The
   pair is therefore grouped into one structure with a magic marker and
   registered with the scrubber (seu_mitigation.c), which keeps a redundant
   copy of it and rewrites it if it changes without the owner saying so.

   It stays in SRAM1 on purpose: the SRAM2 parity NMI handler writes a
   LastStates entry, and the block that just reported a parity error must not
   be on the path that records its own failure. */
#define LASTSTATES_MIRROR_MAGIC  0x4C534D52U   /* "LSMR" */

typedef struct {
    uint32_t magic;   /* LASTSTATES_MIRROR_MAGIC                            */
    uint32_t count;   /* entries written since boot (capped at the pool)    */
    uint32_t idx;     /* next write slot (circular)                         */
} laststates_mirror_t;

/* Address and size of that structure, for seu_mitigation_register_region(). */
void *laststates_mirror_region(size_t *len);

#ifdef HOST_UNIT_TEST
/* Host-test-only: does the Flash page-erase bounds guard accept this address?
   Exposed so the guard that keeps a stray erase away from the vector table and
   the dual-bank golden image can be unit-tested directly. Not compiled into
   the flight image. */
int laststates_erase_addr_allowed(uintptr_t addr);

/* Host-test-only: force the next laststates_pool_lock() results. The flight
   lock depends on CMSIS-RTOS2 and __get_IPSR(), neither of which exists on the
   host, so this is the only way to exercise the LASTSTATES_LOCK_FAILED
   refusal path (Kilo #21) from a test. Reset to LASTSTATES_LOCK_NOT_NEEDED. */
void laststates_pool_lock_set_result_for_test(int result);
#endif

#endif /* MEMORY_H */
