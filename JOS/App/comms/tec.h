#ifndef TEC_H
#define TEC_H

/**
 * @file    tec.h
 * @brief   TEC (Telecommand Execution Control) task dispatch and VarAddr table.
 *
 * SPEC SOURCE — `TT&C/TTC Operations/TTC packets.xlsx` (SharePoint J2050space,
 * walked 2026-09-10). This document has NO release status and carries two
 * conflicting revisions (the active sheet and an `(old)` sheet with a 5-byte
 * INFO field). Every constant below is taken from the ACTIVE sheet; the module
 * is deliberately small and revertible so a revision change is a one-table
 * diff, not a rewrite. See /root/sp_findings/TROVATO.md §4.
 *
 * LAYER — this module sits *above* the TT&C frame layer. The frame layer
 * (`comms.c` / `comms_validate.c`, spec INFO(4)+UNIX(4)+MAC(4)+PL) is
 * responsible for extracting from a received frame the triple
 *
 *     TEC type  (INFO byte 3, bits 1-2)   -> tec_type_t
 *     TEC task  (INFO byte 3, bits 3-8)   -> 6-bit task id
 *     payload   (PL length bytes, 0..100) -> const uint8_t *
 *
 * and handing it to tec_dispatch(). The two layers share NO state: this
 * module never parses a frame, never touches the radio and never mentions the
 * MAC/ECC/UNIX fields.
 *
 * DECOUPLING — the module does NOT call the OBSW state machine, the BMS, the
 * AOCS or the radio directly. Each defined task is bound to a caller-supplied
 * callback (tec_handler_fn) and every variable read/write goes through a
 * caller-supplied tec_var_ops_t. That keeps the wire contract and the
 * permission rules testable on the host with no flight dependency, and keeps
 * the "what a command does" decision in the subsystem that owns it.
 *
 * WHAT IS ENFORCED HERE (and nowhere else):
 *   - the (type, task) pair must be a slot DEFINED in the spec. Slots left
 *     empty in the spec are rejected with TEC_ERR_UNDEFINED_TASK — a dedicated
 *     error, never a silent ignore. This matters because a dropped command is
 *     indistinguishable from a lost uplink on the ground.
 *   - the payload length must match the task's declared shape (exact byte
 *     count, or an N-byte record repeated an integral number of times).
 *   - a variable write must respect the VarAddr permission: writing a
 *     read-only variable is refused with TEC_ERR_READ_ONLY.
 *
 * Refs: NASA Power of Ten #1/#5 (bounded arithmetic, fixed loop bounds),
 *       NASA-STD-8739.8 (unknown/malformed/out-of-range commands are rejected,
 *       never executed).
 */

#include <stddef.h>
#include <stdint.h>

/* ---------- INFO field geometry (spec: TTC packets.xlsx) ---------- */

/** Maximum PL length carried in INFO byte 4 (spec: 0..100). */
#define TEC_PAYLOAD_MAX 100U

/** One Variable-change record: uint16 address + float32 value (spec: 6 B). */
#define TEC_VAR_CHANGE_RECORD_LEN 6U

/* ---------- TEC task types (INFO byte 3, bits 1-2) ---------- */

typedef enum {
    TEC_TYPE_HK  = 1,   /**< Housekeeping        */
    TEC_TYPE_DAQ = 2,   /**< Data Acquisition    */
    TEC_TYPE_PE  = 3,   /**< Payload Execution   */
    TEC_TYPE_DT  = 4,   /**< Data Transfer       */
} tec_type_t;

/* ---------- HK task ids (TEC task field, 6 bits) ---------- */

#define TEC_TASK_HK_OBC_REBOOT   0x01U  /**< 0 B                       */
#define TEC_TASK_HK_EXIT_STATE   0x02U  /**< 2 B: old state, new state */
#define TEC_TASK_HK_VAR_CHANGE   0x03U  /**< 6 B per entry, repeatable */
#define TEC_TASK_HK_SET_TIME     0x04U  /**< 4 B                       */
#define TEC_TASK_HK_EPS_REBOOT   0x08U  /**< 0 B                       */
#define TEC_TASK_HK_ADCS_REBOOT  0x10U  /**< 0 B                       */
#define TEC_TASK_HK_TLE          0x11U  /**< 43 B (layout: 'HK tasks') */
#define TEC_TASK_HK_LORA_STATE   0x18U  /**< 4 B                       */
#define TEC_TASK_HK_LORA_CONFIG  0x19U  /**< 6 B                       */
#define TEC_TASK_HK_LORA_PING    0x1AU  /**< length not given by spec  */
#define TEC_TASK_HK_ACK          0x31U  /**< 1 B                       */
#define TEC_TASK_HK_NACK         0x32U  /**< length TBD in the spec    */
#define TEC_TASK_HK_LORA_LINK    0x33U  /**< length not given by spec  */

/* ---------- Result codes ---------- */

/**
 * Every dispatch / variable access returns one of these. The codes are kept
 * distinct on purpose: "the task does not exist in the spec" and "the task
 * exists but nothing is wired to it" are different ground diagnostics.
 */
typedef enum {
    TEC_OK = 0,                 /**< accepted and handed to the handler   */
    TEC_ERR_NULL_ARG,           /**< NULL pointer / zero-length contract  */
    TEC_ERR_UNKNOWN_TYPE,       /**< TEC type outside {HK,DAQ,PE,DT}      */
    TEC_ERR_UNDEFINED_TASK,     /**< (type,task) is an empty spec slot    */
    TEC_ERR_BAD_LENGTH,         /**< payload length violates the shape    */
    TEC_ERR_NO_HANDLER,         /**< defined task, no callback registered */
    TEC_ERR_UNKNOWN_VAR,        /**< VarAddr not in the table             */
    TEC_ERR_READ_ONLY,          /**< write attempted on an R variable     */
    TEC_ERR_NO_VAR_OPS,         /**< var access with no ops installed     */
} tec_result_t;

/* ---------- Task table ---------- */

/** How a task's payload length is declared in the spec. */
typedef enum {
    TEC_LEN_FIXED = 0,      /**< exactly @c len bytes                     */
    TEC_LEN_MULTIPLE,       /**< @c len -byte records, >=1, integral      */
    TEC_LEN_UNSPECIFIED,    /**< spec gives no length (TBD / not stated)  */
} tec_len_kind_t;

typedef struct {
    uint8_t         type;       /**< tec_type_t                              */
    uint8_t         task;       /**< 6-bit TEC task id                       */
    const char     *name;       /**< spec label, for logs and tests          */
    tec_len_kind_t  len_kind;   /**< shape of the payload                    */
    uint16_t        len;        /**< fixed bytes, or record size if MULTIPLE */
} tec_task_desc_t;

/* ---------- VarAddr table ---------- */

typedef enum {
    TEC_ACCESS_R  = 0,  /**< readable only        */
    TEC_ACCESS_RW = 1,  /**< readable and writable */
} tec_var_access_t;

typedef struct {
    uint16_t         addr;      /**< VarAddr, 1..15 per the spec              */
    const char      *name;      /**< spec label                               */
    tec_var_access_t access;    /**< permission enforced by tec_var_write()   */
} tec_var_desc_t;

/* ---------- Callback interfaces (caller-populated) ---------- */

/**
 * Handler bound to one (type, task).
 *
 * The handler receives the SAME (type, task) it was registered for plus the
 * raw payload; it returns TEC_OK on success or any tec_result_t to report the
 * refusal upstream. It is the handler's job to talk to the subsystem; tec.c
 * never does.
 */
typedef tec_result_t (*tec_handler_fn)(tec_type_t type, uint8_t task,
                                       const uint8_t *payload, size_t len,
                                       void *arg);

/** Read one variable. @p out is non-NULL; return TEC_OK on success. */
typedef tec_result_t (*tec_var_read_fn)(uint16_t addr, float *out, void *arg);

/** Write one variable (already permission-checked by tec_var_write()). */
typedef tec_result_t (*tec_var_write_fn)(uint16_t addr, float value, void *arg);

typedef struct {
    tec_var_read_fn  read;   /**< required for reads                */
    tec_var_write_fn write;  /**< required for writes               */
    void            *arg;    /**< opaque context passed to both     */
} tec_var_ops_t;

/* ---------- Lifecycle ---------- */

/** Clear every registered handler and the variable ops. */
void tec_reset(void);

/**
 * Bind @p fn to the task (type, task) and return TEC_OK.
 * TEC_ERR_UNDEFINED_TASK if the slot is not in the spec (so a wiring mistake
 * cannot create a handler for a task the flight software will never accept),
 * TEC_ERR_NULL_ARG if @p fn is NULL. Re-registering a task replaces it.
 */
tec_result_t tec_register_handler(tec_type_t type, uint8_t task,
                                  tec_handler_fn fn, void *arg);

/**
 * Register the built-in handlers. Today that is HK Variable change (0x03),
 * whose behaviour is pure VarAddr data access and therefore generic: it walks
 * the payload as repeated [uint16 address big-endian][float32 value
 * big-endian] records and applies each through tec_var_write(), so the R/RW
 * permission rule is enforced in one place. No subsystem knowledge is needed.
 */
void tec_register_default_handlers(void);

/* ---------- Dispatch ---------- */

/**
 * Validate and dispatch one TEC command.
 *
 * Order of checks: type in range -> (type,task) defined in the spec ->
 * payload length matches the declared shape -> a handler is bound.
 * On success the handler is invoked and its return code is propagated.
 *
 * @p payload may be NULL only when @p len is 0. Lengths above TEC_PAYLOAD_MAX
 * are refused even for tasks whose length the spec leaves open.
 *
 * Returns TEC_OK, or the first failure encountered.
 */
tec_result_t tec_dispatch(tec_type_t type, uint8_t task,
                          const uint8_t *payload, size_t len);

/**
 * Built-in HK 0x03 body, exposed for direct use and for the tests.
 *
 * @p len must be a non-zero multiple of TEC_VAR_CHANGE_RECORD_LEN and at most
 * TEC_PAYLOAD_MAX. Decoding is big-endian byte assembly (never a cast), so it
 * is correct on any host and does not depend on struct layout. Stops at the
 * first record the permission rules refuse and returns that error, so a
 * rejected record is never silently skipped.
 */
tec_result_t tec_hk_variable_change(const uint8_t *payload, size_t len);

/* ---------- Variable access ---------- */

/** Install (or, with NULL, remove) the variable accessor callbacks. */
void tec_set_var_ops(const tec_var_ops_t *ops);

/** Read @p addr into @p out. TEC_ERR_UNKNOWN_VAR if not in the table. */
tec_result_t tec_var_read(uint16_t addr, float *out);

/**
 * Write @p value to @p addr.
 * TEC_ERR_UNKNOWN_VAR if not in the table, TEC_ERR_READ_ONLY if the variable
 * is declared R, TEC_ERR_NO_VAR_OPS if no write callback is installed.
 */
tec_result_t tec_var_write(uint16_t addr, float value);

/* ---------- Introspection (tests, telemetry, docs) ---------- */

/** Descriptor for (type, task), or NULL when the slot is undefined. */
const tec_task_desc_t *tec_task_lookup(tec_type_t type, uint8_t task);

/** Number of tasks defined in the spec table. */
size_t tec_task_count(void);

/** Descriptor at index @p idx (0..tec_task_count()-1), else NULL. */
const tec_task_desc_t *tec_task_at(size_t idx);

/** Descriptor for @p addr, or NULL when not in the table. */
const tec_var_desc_t *tec_var_lookup(uint16_t addr);

/** Number of variables defined in the table. */
size_t tec_var_count(void);

/** Descriptor at index @p idx (0..tec_var_count()-1), else NULL. */
const tec_var_desc_t *tec_var_at(size_t idx);

#endif /* TEC_H */
