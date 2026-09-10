/**
 * @file    tec.c
 * @brief   TEC task dispatch and VarAddr permission table (see tec.h).
 *
 * Spec source: `TT&C/TTC Operations/TTC packets.xlsx` (unreleased, active
 * sheet). This file is the single place where the wire contract lives; adding
 * a command is one row in TEC_TASKS plus a handler registration, and changing
 * a length is one integer. No other translation unit needs to change.
 *
 * Deliberately dependency-free: only <stdint.h>/<stddef.h>/<string.h>. That is
 * what lets the whole contract run under Ceedling on the host.
 */

#include "tec.h"

#include <string.h>

/* ========================================================================== */
/* Spec tables                                                                */
/* ========================================================================== */

/*
 * Defined TEC tasks. ONLY the HK tasks are enumerated by the spec today; the
 * DAQ/PE/DT types exist (INFO bits) but carry no task definition, so every
 * DAQ/PE/DT task is an empty slot and is rejected as such.
 *
 * Length shapes:
 *   0x01 OBC reboot   0 B
 *   0x02 Exit state   2 B  (old state, new state)
 *   0x03 Var change   6 B x N  (address uint16 BE + value float32 BE)
 *   0x04 Set time     4 B
 *   0x08 EPS reboot   0 B
 *   0x10 ADCS reboot  0 B
 *   0x11 TLE          43 B  (field layout is in the 'HK tasks' sheet and is
 *                            NOT decoded here — this module only enforces the
 *                            length and forwards the payload)
 *   0x18 LoRa state   4 B  (byte 1 TX state, bits 2-4 duration seconds)
 *   0x19 LoRa config  6 B  (bytes 1-4 frequency Hz, bits 33-36 bandwidth,
 *                            bits 37-40 SF, byte 6 duration)
 *   0x1A LoRa ping    length NOT stated by the spec
 *   0x31 ACK          1 B
 *   0x32 NACK         length TBD in the spec
 *   0x33 LoRa link    length NOT stated by the spec
 *
 * The three "not stated" rows are marked TEC_LEN_UNSPECIFIED: they accept any
 * length up to TEC_PAYLOAD_MAX but are listed here so they are DISTINGUISHABLE
 * from an undefined task. That distinction is the point — an unimplemented
 * length must not turn into a silently dropped command. Pinning these three
 * is an open item for the TT&C subteam.
 */
static const tec_task_desc_t TEC_TASKS[] = {
    { TEC_TYPE_HK, TEC_TASK_HK_OBC_REBOOT,  "OBC reboot",      TEC_LEN_FIXED,       0U  },
    { TEC_TYPE_HK, TEC_TASK_HK_EXIT_STATE,  "Exit state",      TEC_LEN_FIXED,       2U  },
    { TEC_TYPE_HK, TEC_TASK_HK_VAR_CHANGE,  "Variable change", TEC_LEN_MULTIPLE,    TEC_VAR_CHANGE_RECORD_LEN },
    { TEC_TYPE_HK, TEC_TASK_HK_SET_TIME,    "Set time",        TEC_LEN_FIXED,       4U  },
    { TEC_TYPE_HK, TEC_TASK_HK_EPS_REBOOT,  "EPS reboot",      TEC_LEN_FIXED,       0U  },
    { TEC_TYPE_HK, TEC_TASK_HK_ADCS_REBOOT, "ADCS reboot",     TEC_LEN_FIXED,       0U  },
    { TEC_TYPE_HK, TEC_TASK_HK_TLE,         "TLE",             TEC_LEN_FIXED,      43U  },
    { TEC_TYPE_HK, TEC_TASK_HK_LORA_STATE,  "LoRa state",      TEC_LEN_FIXED,       4U  },
    { TEC_TYPE_HK, TEC_TASK_HK_LORA_CONFIG, "LoRa config",     TEC_LEN_FIXED,       6U  },
    { TEC_TYPE_HK, TEC_TASK_HK_LORA_PING,   "LoRa ping",       TEC_LEN_UNSPECIFIED, 0U  },
    { TEC_TYPE_HK, TEC_TASK_HK_ACK,         "ACK",             TEC_LEN_FIXED,       1U  },
    { TEC_TYPE_HK, TEC_TASK_HK_NACK,        "NACK",            TEC_LEN_UNSPECIFIED, 0U  },
    { TEC_TYPE_HK, TEC_TASK_HK_LORA_LINK,   "LoRa link",       TEC_LEN_UNSPECIFIED, 0U  },
};

#define TEC_TASK_COUNT (sizeof(TEC_TASKS) / sizeof(TEC_TASKS[0]))

/*
 * VarAddr table — the only addresses a TEC Variable-change / read may touch.
 * 1..13 are read-only telemetry; 14 (OBC_STATE) and 15 (AOCS_STATE) are
 * read/write. A write to anything else is refused.
 */
static const tec_var_desc_t TEC_VARS[] = {
    {  1U, "SoC",          TEC_ACCESS_R  },
    {  2U, "Vbat",         TEC_ACCESS_R  },
    {  3U, "Vbat1",        TEC_ACCESS_R  },
    {  4U, "Vbat2",        TEC_ACCESS_R  },
    {  5U, "Vbat3",        TEC_ACCESS_R  },
    {  6U, "Vbat4",        TEC_ACCESS_R  },
    {  7U, "V_in",         TEC_ACCESS_R  },
    {  8U, "V_out",        TEC_ACCESS_R  },
    {  9U, "POK_CLOUD",    TEC_ACCESS_R  },
    { 10U, "POK_CRYSTALS", TEC_ACCESS_R  },
    { 11U, "POK_AOCS",     TEC_ACCESS_R  },
    { 12U, "POK_MAGN",     TEC_ACCESS_R  },
    { 13U, "POK_LED",      TEC_ACCESS_R  },
    { 14U, "OBC_STATE",    TEC_ACCESS_RW },
    { 15U, "AOCS_STATE",   TEC_ACCESS_RW },
};

#define TEC_VAR_COUNT (sizeof(TEC_VARS) / sizeof(TEC_VARS[0]))

/* ========================================================================== */
/* Caller-populated state                                                     */
/* ========================================================================== */

/* One binding slot per defined task, so a binding always has a valid home and
 * there is no "table full" failure mode to reason about. */
typedef struct {
    tec_handler_fn fn;
    void          *arg;
} tec_binding_t;

static tec_binding_t s_bindings[TEC_TASK_COUNT];
static tec_var_ops_t s_var_ops;

/* ========================================================================== */
/* Lookup helpers                                                             */
/* ========================================================================== */

/** Index of (type, task) in TEC_TASKS, or TEC_TASK_COUNT when undefined. */
static size_t tec_task_index(tec_type_t type, uint8_t task)
{
    size_t i;

    for (i = 0U; i < TEC_TASK_COUNT; i++) {
        if ((TEC_TASKS[i].type == (uint8_t)type) && (TEC_TASKS[i].task == task)) {
            return i;
        }
    }
    return TEC_TASK_COUNT;
}

const tec_task_desc_t *tec_task_lookup(tec_type_t type, uint8_t task)
{
    size_t i = tec_task_index(type, task);

    return (i < TEC_TASK_COUNT) ? &TEC_TASKS[i] : NULL;
}

size_t tec_task_count(void)
{
    return TEC_TASK_COUNT;
}

const tec_task_desc_t *tec_task_at(size_t idx)
{
    return (idx < TEC_TASK_COUNT) ? &TEC_TASKS[idx] : NULL;
}

const tec_var_desc_t *tec_var_lookup(uint16_t addr)
{
    size_t i;

    for (i = 0U; i < TEC_VAR_COUNT; i++) {
        if (TEC_VARS[i].addr == addr) {
            return &TEC_VARS[i];
        }
    }
    return NULL;
}

size_t tec_var_count(void)
{
    return TEC_VAR_COUNT;
}

const tec_var_desc_t *tec_var_at(size_t idx)
{
    return (idx < TEC_VAR_COUNT) ? &TEC_VARS[idx] : NULL;
}

/* ========================================================================== */
/* Lifecycle                                                                  */
/* ========================================================================== */

void tec_reset(void)
{
    size_t i;

    for (i = 0U; i < TEC_TASK_COUNT; i++) {
        s_bindings[i].fn  = NULL;
        s_bindings[i].arg = NULL;
    }
    s_var_ops.read  = NULL;
    s_var_ops.write = NULL;
    s_var_ops.arg   = NULL;
}

tec_result_t tec_register_handler(tec_type_t type, uint8_t task,
                                  tec_handler_fn fn, void *arg)
{
    size_t i;

    if (fn == NULL) {
        return TEC_ERR_NULL_ARG;
    }
    i = tec_task_index(type, task);
    if (i >= TEC_TASK_COUNT) {
        return TEC_ERR_UNDEFINED_TASK;
    }
    s_bindings[i].fn  = fn;
    s_bindings[i].arg = arg;
    return TEC_OK;
}

/* ========================================================================== */
/* Variable access                                                            */
/* ========================================================================== */

void tec_set_var_ops(const tec_var_ops_t *ops)
{
    if (ops == NULL) {
        s_var_ops.read  = NULL;
        s_var_ops.write = NULL;
        s_var_ops.arg   = NULL;
        return;
    }
    s_var_ops = *ops;   /* copy: the caller's struct may be stack-local */
}

tec_result_t tec_var_read(uint16_t addr, float *out)
{
    if (out == NULL) {
        return TEC_ERR_NULL_ARG;
    }
    if (tec_var_lookup(addr) == NULL) {
        return TEC_ERR_UNKNOWN_VAR;
    }
    if (s_var_ops.read == NULL) {
        return TEC_ERR_NO_VAR_OPS;
    }
    return s_var_ops.read(addr, out, s_var_ops.arg);
}

tec_result_t tec_var_write(uint16_t addr, float value)
{
    const tec_var_desc_t *desc = tec_var_lookup(addr);

    if (desc == NULL) {
        return TEC_ERR_UNKNOWN_VAR;
    }
    if (desc->access != TEC_ACCESS_RW) {
        return TEC_ERR_READ_ONLY;
    }
    if (s_var_ops.write == NULL) {
        return TEC_ERR_NO_VAR_OPS;
    }
    return s_var_ops.write(addr, value, s_var_ops.arg);
}

/* ========================================================================== */
/* Built-in handlers                                                          */
/* ========================================================================== */

/* Assemble a 32-bit big-endian word from four wire bytes. Done byte-wise on
 * purpose: no cast, no alignment assumption, correct on any host. */
static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/* Assemble a 16-bit big-endian word from two wire bytes. */
static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/* Pre-validation pass over the metadata of every record, run BEFORE the first
 * write so the whole command is applied all-or-nothing. Only the parts that do
 * not depend on the apply actually happening are checked: the address must be
 * in the VarAddr table, it must be writable (RW), and a write sink must be
 * installed. This mirrors exactly what tec_var_write() would enforce, minus the
 * call itself, so the pre-pass cannot reject what tec_var_write would accept.
 *
 * The VALUE is deliberately not inspected: no range check, no NaN/Inf test.
 * That contract lives with the owning subsystem and is documented in tec.h. */
static tec_result_t validate_var_records(const uint8_t *payload, size_t len)
{
    size_t off;

    for (off = 0U; off < len; off += (size_t)TEC_VAR_CHANGE_RECORD_LEN) {
        uint16_t              addr = read_be16(&payload[off]);
        const tec_var_desc_t *desc = tec_var_lookup(addr);

        if (desc == NULL) {
            return TEC_ERR_UNKNOWN_VAR;
        }
        if (desc->access != TEC_ACCESS_RW) {
            return TEC_ERR_READ_ONLY;
        }
        if (s_var_ops.write == NULL) {
            return TEC_ERR_NO_VAR_OPS;
        }
    }
    return TEC_OK;
}

tec_result_t tec_hk_variable_change(const uint8_t *payload, size_t len)
{
    size_t       off;
    uint32_t     bits;
    float        value;
    tec_result_t rc;

    if (payload == NULL) {
        return TEC_ERR_NULL_ARG;
    }
    if ((len == 0U) || ((len % (size_t)TEC_VAR_CHANGE_RECORD_LEN) != 0U) ||
        (len > (size_t)TEC_PAYLOAD_MAX)) {
        return TEC_ERR_BAD_LENGTH;
    }

    /* All-or-nothing: validate every record's metadata first. A bad record
     * anywhere refuses the command before ANY write is applied. */
    rc = validate_var_records(payload, len);
    if (rc != TEC_OK) {
        return rc;
    }

    for (off = 0U; off < len; off += (size_t)TEC_VAR_CHANGE_RECORD_LEN) {
        uint16_t addr = read_be16(&payload[off]);

        bits = read_be32(&payload[off + 2U]);
        /* memcpy, not a pointer cast: the bits ARE the IEEE-754 value here
         * (spec says float32), and this avoids a strict-aliasing/alignment
         * violation that a cast would introduce. */
        memcpy(&value, &bits, sizeof value);

        rc = tec_var_write(addr, value);
        if (rc != TEC_OK) {
            return rc;   /* defensive: the pre-pass already cleared this path */
        }
    }
    return TEC_OK;
}

/* Adapter between the generic handler signature and tec_hk_variable_change(),
 * which needs neither the type/task echo nor a context. */
static tec_result_t tec_var_change_handler(tec_type_t type, uint8_t task,
                                           const uint8_t *payload, size_t len,
                                           void *arg)
{
    (void)type;
    (void)task;
    (void)arg;
    return tec_hk_variable_change(payload, len);
}

/* ========================================================================== */
/* Dispatch                                                                   */
/* ========================================================================== */

static tec_result_t validate_length(const tec_task_desc_t *desc,
                                    const uint8_t *payload, size_t len)
{
    if ((payload == NULL) && (len != 0U)) {
        return TEC_ERR_NULL_ARG;
    }
    if (len > (size_t)TEC_PAYLOAD_MAX) {
        return TEC_ERR_BAD_LENGTH;
    }

    switch (desc->len_kind) {
    case TEC_LEN_FIXED:
        if (len != (size_t)desc->len) {
            return TEC_ERR_BAD_LENGTH;
        }
        break;

    case TEC_LEN_MULTIPLE:
        if (desc->len == 0U) {                 /* table misconfiguration */
            return TEC_ERR_BAD_LENGTH;
        }
        if (len == 0U) {                       /* no record at all      */
            return TEC_ERR_BAD_LENGTH;
        }
        if ((len % (size_t)desc->len) != 0U) { /* partial trailing record */
            return TEC_ERR_BAD_LENGTH;
        }
        break;

    case TEC_LEN_UNSPECIFIED:
    default:
        break;
    }
    return TEC_OK;
}

static tec_result_t validate_command(tec_type_t type, uint8_t task,
                                     const uint8_t *payload, size_t len,
                                     size_t *out_index)
{
    int    t = (int)type;

    /* 2-bit field, but only 1..4 are assigned by the spec. */
    if ((t < (int)TEC_TYPE_HK) || (t > (int)TEC_TYPE_DT)) {
        return TEC_ERR_UNKNOWN_TYPE;
    }

    *out_index = tec_task_index(type, task);
    if (*out_index >= TEC_TASK_COUNT) {
        return TEC_ERR_UNDEFINED_TASK;
    }

    return validate_length(&TEC_TASKS[*out_index], payload, len);
}

tec_result_t tec_dispatch(tec_type_t type, uint8_t task,
                          const uint8_t *payload, size_t len)
{
    size_t       idx = 0U;
    tec_result_t rc;

    rc = validate_command(type, task, payload, len, &idx);
    if (rc != TEC_OK) {
        return rc;
    }

    if (s_bindings[idx].fn == NULL) {
        return TEC_ERR_NO_HANDLER;
    }
    return s_bindings[idx].fn(type, task, payload, len, s_bindings[idx].arg);
}

void tec_register_default_handlers(void)
{
    (void)tec_register_handler(TEC_TYPE_HK, TEC_TASK_HK_VAR_CHANGE,
                               tec_var_change_handler, NULL);
}
