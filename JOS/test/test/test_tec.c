/**
 * @file    test_tec.c
 * @brief   Unit tests for the TEC dispatch and the VarAddr table
 *          (App/comms/tec.c, spec: TT&C/TTC Operations/TTC packets.xlsx).
 *
 * What is verified, and why it is not a tautology
 *   - the ID MAP is asserted against NUMERIC LITERALS typed here (0x01, 0x02,
 *     ... 0x33) and literal names, never against the TEC_TASK_HK_* macros the
 *     module itself defines. If someone renumbers a task in tec.c without
 *     touching the spec, this test fails; a test written with the macros would
 *     keep passing and assert nothing.
 *   - a defined task with the declared payload is accepted and the bound
 *     handler receives exactly (type, task, payload, len);
 *   - a wrong payload length is refused with TEC_ERR_BAD_LENGTH (shorter AND
 *     longer, including one byte past a legal multiple);
 *   - a task that is an EMPTY SLOT in the spec is refused with the dedicated
 *     TEC_ERR_UNDEFINED_TASK — never ignored;
 *   - a write to a read-only VarAddr is refused AND the write callback is not
 *     reached; a write to an R/W VarAddr is accepted and reaches the callback;
 *   - a Variable-change command with several records is ALL-OR-NOTHING: if any
 *     record is bad (unknown address, read-only, no sink) NOT ONE record is
 *     applied, so the ground's error and the subsystem state agree;
 *   - tec_var_write() validates the address and the permission but NOT the
 *     value: 1e30f, NaN and Inf reach the callback verbatim (documented
 *     contract — the owning subsystem owns the admissible range);
 *   - the type ID column (1..4, the enum) and the wire Bin ID column (0..3,
 *     TEC_WIRE_TYPE_*) are pinned side by side, because they differ (PE = 3 vs
 *     wire 2) and the frame layer must use the wire one;
 *   - the Variable-change record decode is big-endian: the payload is built
 *     from explicit bytes, so correctness does not depend on host endianness.
 *
 * The subsystem actions are doubled by plain C function pointers, which is
 * exactly how the module is meant to be decoupled — no CMock, no HAL, no
 * state machine.
 */

#include "unity.h"
#include "tec.h"

#include <string.h>

/* ========================================================================== */
/* Doubles                                                                    */
/* ========================================================================== */

static int             g_handler_calls;
static tec_type_t      g_last_type;
static uint8_t         g_last_task;
static const uint8_t  *g_last_payload;
static size_t          g_last_len;
static tec_result_t    g_handler_rc;

static tec_result_t recording_handler(tec_type_t type, uint8_t task,
                                      const uint8_t *payload, size_t len,
                                      void *arg)
{
    (void)arg;
    g_handler_calls++;
    g_last_type    = type;
    g_last_task    = task;
    g_last_payload = payload;
    g_last_len     = len;
    return g_handler_rc;
}

static int          g_var_read_calls;
static int          g_var_write_calls;
static uint16_t     g_last_var_addr;
static float        g_last_var_value;
static tec_result_t g_var_read_rc;
static tec_result_t g_var_write_rc;

/* The read value is derived from the address, so a test can tell a real read
 * of address N from a read of any other address. */
static tec_result_t fake_var_read(uint16_t addr, float *out, void *arg)
{
    (void)arg;
    g_var_read_calls++;
    g_last_var_addr = addr;
    *out = (float)addr;
    return g_var_read_rc;
}

static tec_result_t fake_var_write(uint16_t addr, float value, void *arg)
{
    (void)arg;
    g_var_write_calls++;
    g_last_var_addr  = addr;
    g_last_var_value = value;
    return g_var_write_rc;
}

static const tec_var_ops_t FAKE_VAR_OPS = {
    .read  = fake_var_read,
    .write = fake_var_write,
    .arg   = NULL,
};

void setUp(void)
{
    tec_reset();
    tec_set_var_ops(&FAKE_VAR_OPS);

    g_handler_calls  = 0;
    g_last_type      = (tec_type_t)0;
    g_last_task      = 0U;
    g_last_payload   = NULL;
    g_last_len       = 0U;
    g_handler_rc     = TEC_OK;

    g_var_read_calls = 0;
    g_var_write_calls = 0;
    g_last_var_addr  = 0U;
    g_last_var_value = 0.0f;
    g_var_read_rc    = TEC_OK;
    g_var_write_rc   = TEC_OK;
}

void tearDown(void)
{
    tec_reset();
}

/* ========================================================================== */
/* Spec map — literals only, straight from TTC packets.xlsx                   */
/* ========================================================================== */

typedef struct {
    uint8_t         id;
    const char     *name;
    tec_len_kind_t  kind;
    uint16_t        len;
} spec_task_t;

static const spec_task_t SPEC_HK[] = {
    { 0x01U, "OBC reboot",      TEC_LEN_FIXED,        0U },
    { 0x02U, "Exit state",      TEC_LEN_FIXED,        2U },
    { 0x03U, "Variable change", TEC_LEN_MULTIPLE,     6U },
    { 0x04U, "Set time",        TEC_LEN_FIXED,        4U },
    { 0x08U, "EPS reboot",      TEC_LEN_FIXED,        0U },
    { 0x10U, "ADCS reboot",     TEC_LEN_FIXED,        0U },
    { 0x11U, "TLE",             TEC_LEN_FIXED,       43U },
    { 0x18U, "LoRa state",      TEC_LEN_FIXED,        4U },
    { 0x19U, "LoRa config",     TEC_LEN_FIXED,        6U },
    { 0x1AU, "LoRa ping",       TEC_LEN_UNSPECIFIED,  0U },
    { 0x31U, "ACK",             TEC_LEN_FIXED,        1U },
    { 0x32U, "NACK",            TEC_LEN_UNSPECIFIED,  0U },
    { 0x33U, "LoRa link",       TEC_LEN_UNSPECIFIED,  0U },
};

#define SPEC_HK_COUNT (sizeof(SPEC_HK) / sizeof(SPEC_HK[0]))

/* Every HK task defined by the spec, with its exact id, name and length. */
void test_task_map_matches_spec_literals(void)
{
    size_t i;

    TEST_ASSERT_EQUAL_UINT(SPEC_HK_COUNT, tec_task_count());

    for (i = 0U; i < SPEC_HK_COUNT; i++) {
        const tec_task_desc_t *d = tec_task_lookup(TEC_TYPE_HK, SPEC_HK[i].id);

        TEST_ASSERT_NOT_NULL(d);
        if (d == NULL) {
            continue;   /* keep the rest of the diagnostics coming */
        }
        TEST_ASSERT_EQUAL_INT((int)TEC_TYPE_HK, (int)d->type);
        TEST_ASSERT_EQUAL_UINT8(SPEC_HK[i].id, d->task);
        TEST_ASSERT_EQUAL_STRING(SPEC_HK[i].name, d->name);
        TEST_ASSERT_EQUAL_INT((int)SPEC_HK[i].kind, (int)d->len_kind);
        TEST_ASSERT_EQUAL_UINT16(SPEC_HK[i].len, d->len);
    }
}

/* Numeric check of the literal ids themselves (0x01..0x33), independent of
 * the table order and of the names. */
void test_task_ids_are_the_literal_spec_values(void)
{
    TEST_ASSERT_EQUAL_UINT8(0x01U, tec_task_lookup(TEC_TYPE_HK, 0x01U)->task);
    TEST_ASSERT_EQUAL_UINT8(0x02U, tec_task_lookup(TEC_TYPE_HK, 0x02U)->task);
    TEST_ASSERT_EQUAL_UINT8(0x03U, tec_task_lookup(TEC_TYPE_HK, 0x03U)->task);
    TEST_ASSERT_EQUAL_UINT8(0x04U, tec_task_lookup(TEC_TYPE_HK, 0x04U)->task);
    TEST_ASSERT_EQUAL_UINT8(0x08U, tec_task_lookup(TEC_TYPE_HK, 0x08U)->task);
    TEST_ASSERT_EQUAL_UINT8(0x10U, tec_task_lookup(TEC_TYPE_HK, 0x10U)->task);
    TEST_ASSERT_EQUAL_UINT8(0x11U, tec_task_lookup(TEC_TYPE_HK, 0x11U)->task);
    TEST_ASSERT_EQUAL_UINT8(0x18U, tec_task_lookup(TEC_TYPE_HK, 0x18U)->task);
    TEST_ASSERT_EQUAL_UINT8(0x19U, tec_task_lookup(TEC_TYPE_HK, 0x19U)->task);
    TEST_ASSERT_EQUAL_UINT8(0x1AU, tec_task_lookup(TEC_TYPE_HK, 0x1AU)->task);
    TEST_ASSERT_EQUAL_UINT8(0x31U, tec_task_lookup(TEC_TYPE_HK, 0x31U)->task);
    TEST_ASSERT_EQUAL_UINT8(0x32U, tec_task_lookup(TEC_TYPE_HK, 0x32U)->task);
    TEST_ASSERT_EQUAL_UINT8(0x33U, tec_task_lookup(TEC_TYPE_HK, 0x33U)->task);
}

/* Slots the spec does NOT define must not resolve to a task. */
void test_undefined_task_ids_have_no_descriptor(void)
{
    static const uint8_t holes[] = {
        0x00U, 0x05U, 0x06U, 0x07U, 0x09U, 0x0AU, 0x12U, 0x20U, 0x3FU,
    };
    size_t i;

    for (i = 0U; i < (sizeof(holes) / sizeof(holes[0])); i++) {
        TEST_ASSERT_NULL(tec_task_lookup(TEC_TYPE_HK, holes[i]));
    }
}

/* Task types DAQ/PE/DT exist in the spec but carry no task definition, so
 * every task under them is an empty slot. */
void test_daq_pe_dt_carry_no_tasks(void)
{
    static const tec_type_t types[] = { TEC_TYPE_DAQ, TEC_TYPE_PE, TEC_TYPE_DT };
    size_t i;

    for (i = 0U; i < (sizeof(types) / sizeof(types[0])); i++) {
        TEST_ASSERT_NULL(tec_task_lookup(types[i], 0x01U));
        TEST_ASSERT_EQUAL_INT(TEC_ERR_UNDEFINED_TASK,
                              tec_dispatch(types[i], 0x01U, NULL, 0U));
    }
}

/* ========================================================================== */
/* Dispatch — accept path                                                     */
/* ========================================================================== */

void test_defined_task_correct_payload_accepted(void)
{
    uint8_t payload[2] = { 0x02U, 0x03U };

    TEST_ASSERT_EQUAL_INT(TEC_OK, tec_register_handler(TEC_TYPE_HK, 0x01U,
                                                       recording_handler, NULL));
    TEST_ASSERT_EQUAL_INT(TEC_OK, tec_register_handler(TEC_TYPE_HK, 0x02U,
                                                       recording_handler, NULL));

    /* zero-length task, no payload pointer required */
    TEST_ASSERT_EQUAL_INT(TEC_OK, tec_dispatch(TEC_TYPE_HK, 0x01U, NULL, 0U));
    TEST_ASSERT_EQUAL_INT(1, g_handler_calls);
    TEST_ASSERT_EQUAL_UINT8(0x01U, g_last_task);

    /* 2-byte task with the exact length */
    TEST_ASSERT_EQUAL_INT(TEC_OK, tec_dispatch(TEC_TYPE_HK, 0x02U,
                                               payload, sizeof payload));
    TEST_ASSERT_EQUAL_INT(2, g_handler_calls);
}

void test_handler_receives_exact_arguments(void)
{
    uint8_t payload[43];

    memset(payload, 0xA5, sizeof payload);
    TEST_ASSERT_EQUAL_INT(TEC_OK, tec_register_handler(TEC_TYPE_HK, 0x11U,
                                                       recording_handler, NULL));
    TEST_ASSERT_EQUAL_INT(TEC_OK, tec_dispatch(TEC_TYPE_HK, 0x11U,
                                               payload, sizeof payload));

    TEST_ASSERT_EQUAL_INT(1, g_handler_calls);
    TEST_ASSERT_EQUAL_INT((int)TEC_TYPE_HK, (int)g_last_type);
    TEST_ASSERT_EQUAL_UINT8(0x11U, g_last_task);
    TEST_ASSERT_EQUAL_UINT(43U, (unsigned)g_last_len);
    TEST_ASSERT_EQUAL_PTR(payload, g_last_payload);
}

/* The handler's refusal is propagated verbatim: a rejected command must not
 * come back as TEC_OK. */
void test_handler_return_code_is_propagated(void)
{
    TEST_ASSERT_EQUAL_INT(TEC_OK, tec_register_handler(TEC_TYPE_HK, 0x04U,
                                                       recording_handler, NULL));
    g_handler_rc = TEC_ERR_READ_ONLY;
    TEST_ASSERT_EQUAL_INT(TEC_ERR_READ_ONLY,
                          tec_dispatch(TEC_TYPE_HK, 0x04U, (const uint8_t *)"", 4U));
}

/* A defined task with nothing bound is NOT the same as an undefined task. */
void test_defined_task_without_handler_is_reported(void)
{
    TEST_ASSERT_EQUAL_INT(TEC_ERR_NO_HANDLER,
                          tec_dispatch(TEC_TYPE_HK, 0x01U, NULL, 0U));
}

/* ========================================================================== */
/* Dispatch — length rejection                                                */
/* ========================================================================== */

void test_wrong_fixed_length_rejected(void)
{
    uint8_t payload[64];
    size_t  i;

    memset(payload, 0, sizeof payload);
    TEST_ASSERT_EQUAL_INT(TEC_OK, tec_register_handler(TEC_TYPE_HK, 0x02U,
                                                       recording_handler, NULL));

    /* 0x02 declares 2 bytes: 0, 1 and 3 are all wrong */
    TEST_ASSERT_EQUAL_INT(TEC_ERR_BAD_LENGTH, tec_dispatch(TEC_TYPE_HK, 0x02U, payload, 0U));
    TEST_ASSERT_EQUAL_INT(TEC_ERR_BAD_LENGTH, tec_dispatch(TEC_TYPE_HK, 0x02U, payload, 1U));
    TEST_ASSERT_EQUAL_INT(TEC_ERR_BAD_LENGTH, tec_dispatch(TEC_TYPE_HK, 0x02U, payload, 3U));

    /* 0x01 declares 0 bytes: anything non-zero is wrong */
    TEST_ASSERT_EQUAL_INT(TEC_ERR_BAD_LENGTH, tec_dispatch(TEC_TYPE_HK, 0x01U, payload, 1U));

    /* 0x11 declares 43 bytes: one short and one long both fail */
    TEST_ASSERT_EQUAL_INT(TEC_ERR_BAD_LENGTH, tec_dispatch(TEC_TYPE_HK, 0x11U, payload, 42U));
    TEST_ASSERT_EQUAL_INT(TEC_ERR_BAD_LENGTH, tec_dispatch(TEC_TYPE_HK, 0x11U, payload, 44U));

    /* every rejection above must have stopped before the handler */
    TEST_ASSERT_EQUAL_INT(0, g_handler_calls);

    /* and none of the wrong lengths may have been silently ignored */
    for (i = 0U; i < 64U; i++) {
        if (i == 2U) {
            continue;
        }
        if (i == 0U) {
            continue;   /* 0x01 path below, not 0x02 */
        }
        TEST_ASSERT_NOT_EQUAL(TEC_OK, tec_dispatch(TEC_TYPE_HK, 0x02U, payload, i));
    }
}

/* Variable change is 6 B per record, repeated an integral number of times. */
void test_variable_change_requires_integral_records(void)
{
    /* eight records, each: address 14 (R/W), value 1.0f */
    static uint8_t payload[48] = {
        0x00U, 0x0EU, 0x3FU, 0x80U, 0x00U, 0x00U,
        0x00U, 0x0EU, 0x3FU, 0x80U, 0x00U, 0x00U,
        0x00U, 0x0EU, 0x3FU, 0x80U, 0x00U, 0x00U,
        0x00U, 0x0EU, 0x3FU, 0x80U, 0x00U, 0x00U,
        0x00U, 0x0EU, 0x3FU, 0x80U, 0x00U, 0x00U,
        0x00U, 0x0EU, 0x3FU, 0x80U, 0x00U, 0x00U,
        0x00U, 0x0EU, 0x3FU, 0x80U, 0x00U, 0x00U,
        0x00U, 0x0EU, 0x3FU, 0x80U, 0x00U, 0x00U,
    };

    tec_register_default_handlers();

    TEST_ASSERT_EQUAL_INT(TEC_OK, tec_dispatch(TEC_TYPE_HK, 0x03U, payload, 6U));
    TEST_ASSERT_EQUAL_INT(TEC_OK, tec_dispatch(TEC_TYPE_HK, 0x03U, payload, 12U));

    TEST_ASSERT_EQUAL_INT(TEC_ERR_BAD_LENGTH, tec_dispatch(TEC_TYPE_HK, 0x03U, payload, 0U));
    TEST_ASSERT_EQUAL_INT(TEC_ERR_BAD_LENGTH, tec_dispatch(TEC_TYPE_HK, 0x03U, payload, 5U));
    TEST_ASSERT_EQUAL_INT(TEC_ERR_BAD_LENGTH, tec_dispatch(TEC_TYPE_HK, 0x03U, payload, 7U));
    TEST_ASSERT_EQUAL_INT(TEC_ERR_BAD_LENGTH, tec_dispatch(TEC_TYPE_HK, 0x03U, payload, 13U));
}

void test_oversize_payload_rejected(void)
{
    uint8_t payload[128] = { 0 };

    /* PL length in INFO byte 4 is 0..100: 101 must never pass, even for a task
     * whose length the spec leaves open. */
    TEST_ASSERT_EQUAL_INT(TEC_OK, tec_register_handler(TEC_TYPE_HK, 0x1AU,
                                                       recording_handler, NULL));
    TEST_ASSERT_EQUAL_INT(TEC_OK, tec_dispatch(TEC_TYPE_HK, 0x1AU, payload, 100U));
    TEST_ASSERT_EQUAL_INT(TEC_ERR_BAD_LENGTH, tec_dispatch(TEC_TYPE_HK, 0x1AU, payload, 101U));
}

void test_null_payload_with_length_rejected(void)
{
    TEST_ASSERT_EQUAL_INT(TEC_OK, tec_register_handler(TEC_TYPE_HK, 0x02U,
                                                       recording_handler, NULL));
    TEST_ASSERT_EQUAL_INT(TEC_ERR_NULL_ARG,
                          tec_dispatch(TEC_TYPE_HK, 0x02U, NULL, 2U));
}

/* ========================================================================== */
/* Dispatch — undefined task / unknown type                                   */
/* ========================================================================== */

void test_undefined_task_rejected_with_dedicated_code(void)
{
    static const uint8_t holes[] = {
        0x00U, 0x05U, 0x06U, 0x07U, 0x09U, 0x0AU, 0x12U, 0x20U, 0x3FU, 0x40U, 0xFFU,
    };
    size_t i;

    for (i = 0U; i < (sizeof(holes) / sizeof(holes[0])); i++) {
        TEST_ASSERT_EQUAL_INT(TEC_ERR_UNDEFINED_TASK,
                              tec_dispatch(TEC_TYPE_HK, holes[i], NULL, 0U));
    }
}

void test_unknown_type_rejected(void)
{
    TEST_ASSERT_EQUAL_INT(TEC_ERR_UNKNOWN_TYPE,
                          tec_dispatch((tec_type_t)0, 0x01U, NULL, 0U));
    TEST_ASSERT_EQUAL_INT(TEC_ERR_UNKNOWN_TYPE,
                          tec_dispatch((tec_type_t)5, 0x01U, NULL, 0U));
    TEST_ASSERT_EQUAL_INT(TEC_ERR_UNKNOWN_TYPE,
                          tec_dispatch((tec_type_t)7, 0x01U, NULL, 0U));
}

void test_task_types_are_the_literal_spec_values(void)
{
    /* HK=1, DAQ=2, PE=3, DT=4 — asserted as integers, not as the enum. */
    TEST_ASSERT_EQUAL_INT(1, (int)TEC_TYPE_HK);
    TEST_ASSERT_EQUAL_INT(2, (int)TEC_TYPE_DAQ);
    TEST_ASSERT_EQUAL_INT(3, (int)TEC_TYPE_PE);
    TEST_ASSERT_EQUAL_INT(4, (int)TEC_TYPE_DT);
}

/* The 'Task types' sheet carries TWO numberings: the human-facing "ID" column
 * (1..4, what the enum holds) and the 2-bit "Bin ID" column that actually goes
 * on the wire (HK='00', DAQ='01', PE='10', DT='11'). They are not the same
 * number and must not be confused — PE has ID 3 but wire value 2. Pinned here
 * as literals so the frame layer (#89) has a checked mapping to wire against. */
void test_task_type_wire_bin_ids_match_spec_literals(void)
{
    TEST_ASSERT_EQUAL_UINT8(0U, (uint8_t)TEC_WIRE_TYPE_HK);
    TEST_ASSERT_EQUAL_UINT8(1U, (uint8_t)TEC_WIRE_TYPE_DAQ);
    TEST_ASSERT_EQUAL_UINT8(2U, (uint8_t)TEC_WIRE_TYPE_PE);
    TEST_ASSERT_EQUAL_UINT8(3U, (uint8_t)TEC_WIRE_TYPE_DT);

    /* The documented relation between the two columns: wire = ID - 1, i.e.
     * the wire field is the zero-based index of the type. Asserting it makes a
     * future renumbering of either column fail loudly instead of silently
     * shifting every type on the link. */
    TEST_ASSERT_EQUAL_UINT8((uint8_t)((int)TEC_TYPE_HK  - 1), (uint8_t)TEC_WIRE_TYPE_HK);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)((int)TEC_TYPE_DAQ - 1), (uint8_t)TEC_WIRE_TYPE_DAQ);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)((int)TEC_TYPE_PE  - 1), (uint8_t)TEC_WIRE_TYPE_PE);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)((int)TEC_TYPE_DT  - 1), (uint8_t)TEC_WIRE_TYPE_DT);

    /* The two fields fit the spec's claimed widths. */
    TEST_ASSERT_TRUE((uint8_t)TEC_WIRE_TYPE_DT <= 3U);   /* 2 bits  */
    TEST_ASSERT_TRUE((int)TEC_TYPE_DT <= 4);             /* ID col  */
}

/* ========================================================================== */
/* Handler registration                                                       */
/* ========================================================================== */

void test_register_rejects_undefined_task_and_null_handler(void)
{
    TEST_ASSERT_EQUAL_INT(TEC_ERR_UNDEFINED_TASK,
                          tec_register_handler(TEC_TYPE_HK, 0x05U,
                                               recording_handler, NULL));
    TEST_ASSERT_EQUAL_INT(TEC_ERR_UNDEFINED_TASK,
                          tec_register_handler(TEC_TYPE_DAQ, 0x01U,
                                               recording_handler, NULL));
    TEST_ASSERT_EQUAL_INT(TEC_ERR_NULL_ARG,
                          tec_register_handler(TEC_TYPE_HK, 0x01U, NULL, NULL));
}

/* Re-registering the same task replaces the binding rather than failing. */
void test_reregistration_replaces_binding(void)
{
    TEST_ASSERT_EQUAL_INT(TEC_OK, tec_register_handler(TEC_TYPE_HK, 0x01U,
                                                       recording_handler, NULL));
    TEST_ASSERT_EQUAL_INT(TEC_OK, tec_register_handler(TEC_TYPE_HK, 0x01U,
                                                       recording_handler, NULL));
    TEST_ASSERT_EQUAL_INT(TEC_OK, tec_dispatch(TEC_TYPE_HK, 0x01U, NULL, 0U));
    TEST_ASSERT_EQUAL_INT(1, g_handler_calls);
}

/* ========================================================================== */
/* VarAddr table                                                              */
/* ========================================================================== */

typedef struct {
    uint16_t    addr;
    const char *name;
    int         rw;   /* 0 = R, 1 = R/W */
} spec_var_t;

static const spec_var_t SPEC_VARS[] = {
    {  1U, "SoC",          0 },
    {  2U, "Vbat",         0 },
    {  3U, "Vbat1",        0 },
    {  4U, "Vbat2",        0 },
    {  5U, "Vbat3",        0 },
    {  6U, "Vbat4",        0 },
    {  7U, "V_in",         0 },
    {  8U, "V_out",        0 },
    {  9U, "POK_CLOUD",    0 },
    { 10U, "POK_CRYSTALS", 0 },
    { 11U, "POK_AOCS",     0 },
    { 12U, "POK_MAGN",     0 },
    { 13U, "POK_LED",      0 },
    { 14U, "OBC_STATE",    1 },
    { 15U, "AOCS_STATE",   1 },
};

#define SPEC_VAR_COUNT (sizeof(SPEC_VARS) / sizeof(SPEC_VARS[0]))

void test_var_map_matches_spec_literals(void)
{
    size_t i;

    TEST_ASSERT_EQUAL_UINT(SPEC_VAR_COUNT, tec_var_count());

    for (i = 0U; i < SPEC_VAR_COUNT; i++) {
        const tec_var_desc_t *d = tec_var_lookup(SPEC_VARS[i].addr);

        TEST_ASSERT_NOT_NULL(d);
        if (d == NULL) {
            continue;
        }
        TEST_ASSERT_EQUAL_UINT16(SPEC_VARS[i].addr, d->addr);
        TEST_ASSERT_EQUAL_STRING(SPEC_VARS[i].name, d->name);
        TEST_ASSERT_EQUAL_INT(SPEC_VARS[i].rw ? (int)TEC_ACCESS_RW : (int)TEC_ACCESS_R,
                              (int)d->access);
    }

    /* addresses outside 1..15 do not exist */
    TEST_ASSERT_NULL(tec_var_lookup(0U));
    TEST_ASSERT_NULL(tec_var_lookup(16U));
    TEST_ASSERT_NULL(tec_var_lookup(0xFFFFU));

    /* introspection bounds */
    TEST_ASSERT_NOT_NULL(tec_var_at(0U));
    TEST_ASSERT_NULL(tec_var_at(SPEC_VAR_COUNT));
}

void test_var_read_returns_value_and_rejects_unknown(void)
{
    float v = 0.0f;

    TEST_ASSERT_EQUAL_INT(TEC_OK, tec_var_read(1U, &v));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, v);
    TEST_ASSERT_EQUAL_INT(1, g_var_read_calls);

    TEST_ASSERT_EQUAL_INT(TEC_ERR_UNKNOWN_VAR, tec_var_read(0U, &v));
    TEST_ASSERT_EQUAL_INT(TEC_ERR_UNKNOWN_VAR, tec_var_read(99U, &v));
    TEST_ASSERT_EQUAL_INT(TEC_ERR_NULL_ARG, tec_var_read(1U, NULL));
}

void test_var_access_without_ops_is_reported(void)
{
    float v = 0.0f;

    tec_set_var_ops(NULL);
    TEST_ASSERT_EQUAL_INT(TEC_ERR_NO_VAR_OPS, tec_var_read(1U, &v));
    TEST_ASSERT_EQUAL_INT(TEC_ERR_NO_VAR_OPS, tec_var_write(14U, 1.0f));
}

/* The core permission rule: 1..13 are R, 14/15 are R/W. */
void test_write_to_read_only_variable_rejected(void)
{
    size_t i;

    for (i = 0U; i < SPEC_VAR_COUNT; i++) {
        if (SPEC_VARS[i].rw) {
            continue;
        }
        TEST_ASSERT_EQUAL_INT(TEC_ERR_READ_ONLY,
                              tec_var_write(SPEC_VARS[i].addr, 1.0f));
    }

    /* and the refusal happened BEFORE the subsystem callback */
    TEST_ASSERT_EQUAL_INT(0, g_var_write_calls);
}

void test_write_to_rw_variable_accepted(void)
{
    TEST_ASSERT_EQUAL_INT(TEC_OK, tec_var_write(14U, 3.5f));
    TEST_ASSERT_EQUAL_INT(1, g_var_write_calls);
    TEST_ASSERT_EQUAL_UINT16(14U, g_last_var_addr);
    TEST_ASSERT_EQUAL_FLOAT(3.5f, g_last_var_value);

    TEST_ASSERT_EQUAL_INT(TEC_OK, tec_var_write(15U, -2.25f));
    TEST_ASSERT_EQUAL_INT(2, g_var_write_calls);
    TEST_ASSERT_EQUAL_UINT16(15U, g_last_var_addr);
    TEST_ASSERT_EQUAL_FLOAT(-2.25f, g_last_var_value);
}

void test_write_to_unknown_variable_rejected(void)
{
    TEST_ASSERT_EQUAL_INT(TEC_ERR_UNKNOWN_VAR, tec_var_write(0U, 1.0f));
    TEST_ASSERT_EQUAL_INT(TEC_ERR_UNKNOWN_VAR, tec_var_write(16U, 1.0f));
    TEST_ASSERT_EQUAL_INT(0, g_var_write_calls);
}

/* A variable-store refusal must reach the caller unchanged. */
void test_variable_store_error_is_propagated(void)
{
    g_var_write_rc = TEC_ERR_UNKNOWN_VAR;
    TEST_ASSERT_EQUAL_INT(TEC_ERR_UNKNOWN_VAR, tec_var_write(14U, 1.0f));
}

/* Build a float from its raw IEEE-754 bits, so a NaN / Inf can be constructed
 * without relying on host maths library subtleties. */
static float float_from_bits(uint32_t bits)
{
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

/* CONTRACT (documented in tec.h): tec_var_write validates the ADDRESS and the
 * permission, NOT the value. No range check, no NaN/Inf rejection — the value
 * is the owning subsystem's responsibility and is forwarded verbatim. This test
 * pins that contract so it cannot be tightened silently into a half-check that
 * makes the module refuse values the subsystem would have accepted. */
void test_var_write_forwards_value_without_range_or_finite_check(void)
{
    const float nan = float_from_bits(0x7FC00000U);   /* quiet NaN  */
    const float inf = float_from_bits(0x7F800000U);   /* +Inf       */
    uint32_t    got_bits;

    TEST_ASSERT_EQUAL_INT(TEC_OK, tec_var_write(14U, 1e30f));
    TEST_ASSERT_EQUAL_INT(1, g_var_write_calls);
    TEST_ASSERT_EQUAL_FLOAT(1e30f, g_last_var_value);

    TEST_ASSERT_EQUAL_INT(TEC_OK, tec_var_write(14U, nan));
    TEST_ASSERT_EQUAL_INT(2, g_var_write_calls);
    memcpy(&got_bits, &g_last_var_value, sizeof got_bits);
    TEST_ASSERT_EQUAL_HEX32(0x7FC00000U, got_bits);

    TEST_ASSERT_EQUAL_INT(TEC_OK, tec_var_write(15U, inf));
    TEST_ASSERT_EQUAL_INT(3, g_var_write_calls);
    memcpy(&got_bits, &g_last_var_value, sizeof got_bits);
    TEST_ASSERT_EQUAL_HEX32(0x7F800000U, got_bits);
}

/* Same contract, end to end through the 0x03 handler: a NaN value on the wire
 * is decoded and delivered to the subsystem, not swallowed by tec.c. */
void test_variable_change_forwards_non_finite_value(void)
{
    static const uint8_t payload[] = {
        0x00U, 0x0EU, 0x7FU, 0xC0U, 0x00U, 0x00U,   /* addr 14, NaN, 1e30-free path */
    };
    uint32_t got_bits;

    tec_register_default_handlers();
    TEST_ASSERT_EQUAL_INT(TEC_OK,
                          tec_dispatch(TEC_TYPE_HK, 0x03U,
                                       payload, sizeof payload));
    TEST_ASSERT_EQUAL_INT(1, g_var_write_calls);
    TEST_ASSERT_EQUAL_UINT16(14U, g_last_var_addr);
    memcpy(&got_bits, &g_last_var_value, sizeof got_bits);
    TEST_ASSERT_EQUAL_HEX32(0x7FC00000U, got_bits);
}

/* ========================================================================== */
/* Variable change (HK 0x03) end to end                                       */
/* ========================================================================== */

/* Big-endian records, built from explicit bytes:
 *   address 14 (0x000E), value 1.0f  (0x3F800000)
 *   address 15 (0x000F), value 2.5f  (0x40200000)
 * A host-endianness bug would decode 0x0E00 = 3584 and fail the unknown-var
 * check, so this pins the wire order. */
static const uint8_t VAR_CHANGE_OK[] = {
    0x00U, 0x0EU, 0x3FU, 0x80U, 0x00U, 0x00U,
    0x00U, 0x0FU, 0x40U, 0x20U, 0x00U, 0x00U,
};

void test_variable_change_applies_records(void)
{
    tec_register_default_handlers();
    TEST_ASSERT_EQUAL_INT(TEC_OK,
                          tec_dispatch(TEC_TYPE_HK, 0x03U,
                                       VAR_CHANGE_OK, sizeof VAR_CHANGE_OK));

    TEST_ASSERT_EQUAL_INT(2, g_var_write_calls);
    TEST_ASSERT_EQUAL_UINT16(15U, g_last_var_addr);
    TEST_ASSERT_EQUAL_FLOAT(2.5f, g_last_var_value);
}

/* ATOMICITY: a command with an allowed record followed by a refused one must
 * leave the subsystem COMPLETELY untouched. The prior behaviour applied the
 * good record and then failed, so the ground got an error while the subsystem
 * was half-configured and a retry double-applied the first record. Now a single
 * bad record anywhere refuses the whole command up front. */
void test_variable_change_is_all_or_nothing_with_read_only_target(void)
{
    static const uint8_t payload[] = {
        0x00U, 0x0EU, 0x3FU, 0x80U, 0x00U, 0x00U,   /* addr 14, 1.0f : allowed  */
        0x00U, 0x01U, 0x3FU, 0x80U, 0x00U, 0x00U,   /* addr 1,  1.0f : R -> bad */
    };

    tec_register_default_handlers();
    TEST_ASSERT_EQUAL_INT(TEC_ERR_READ_ONLY,
                          tec_dispatch(TEC_TYPE_HK, 0x03U,
                                       payload, sizeof payload));

    /* pre-validation refused the command: NOT ONE record reached the store */
    TEST_ASSERT_EQUAL_INT(0, g_var_write_calls);
}

/* Same all-or-nothing rule when the offending record is an UNKNOWN address,
 * and when it is the FIRST record (so the good one comes afterwards). */
void test_variable_change_is_all_or_nothing_with_unknown_address(void)
{
    static const uint8_t bad_last[] = {
        0x00U, 0x0EU, 0x3FU, 0x80U, 0x00U, 0x00U,   /* addr 14, allowed      */
        0x00U, 0x63U, 0x3FU, 0x80U, 0x00U, 0x00U,   /* addr 99, unknown -> bad */
    };
    static const uint8_t bad_first[] = {
        0x00U, 0x63U, 0x3FU, 0x80U, 0x00U, 0x00U,   /* addr 99, unknown -> bad */
        0x00U, 0x0EU, 0x3FU, 0x80U, 0x00U, 0x00U,   /* addr 14, allowed      */
    };

    tec_register_default_handlers();

    TEST_ASSERT_EQUAL_INT(TEC_ERR_UNKNOWN_VAR,
                          tec_dispatch(TEC_TYPE_HK, 0x03U,
                                       bad_last, sizeof bad_last));
    TEST_ASSERT_EQUAL_INT(0, g_var_write_calls);

    TEST_ASSERT_EQUAL_INT(TEC_ERR_UNKNOWN_VAR,
                          tec_dispatch(TEC_TYPE_HK, 0x03U,
                                       bad_first, sizeof bad_first));
    TEST_ASSERT_EQUAL_INT(0, g_var_write_calls);
}

/* And when no write callback is installed, the whole command is refused before
 * any record is "applied" — the pre-pass checks that the sink exists. */
void test_variable_change_is_all_or_nothing_without_var_ops(void)
{
    static const uint8_t payload[] = {
        0x00U, 0x0EU, 0x3FU, 0x80U, 0x00U, 0x00U,   /* addr 14, allowed */
    };

    tec_register_default_handlers();
    tec_set_var_ops(NULL);
    TEST_ASSERT_EQUAL_INT(TEC_ERR_NO_VAR_OPS,
                          tec_dispatch(TEC_TYPE_HK, 0x03U,
                                       payload, sizeof payload));
    TEST_ASSERT_EQUAL_INT(0, g_var_write_calls);
}

/* A fully valid command still applies every record, in order: the pre-pass
 * must not have turned the happy path into a no-op. */
void test_variable_change_valid_command_applies_every_record(void)
{
    static const uint8_t payload[] = {
        0x00U, 0x0EU, 0x3FU, 0x80U, 0x00U, 0x00U,   /* addr 14, 1.0f  */
        0x00U, 0x0FU, 0x40U, 0x20U, 0x00U, 0x00U,   /* addr 15, 2.5f  */
        0x00U, 0x0EU, 0x40U, 0x40U, 0x00U, 0x00U,   /* addr 14, 3.0f  */
    };

    tec_register_default_handlers();
    TEST_ASSERT_EQUAL_INT(TEC_OK,
                          tec_dispatch(TEC_TYPE_HK, 0x03U,
                                       payload, sizeof payload));
    TEST_ASSERT_EQUAL_INT(3, g_var_write_calls);
    TEST_ASSERT_EQUAL_UINT16(14U, g_last_var_addr);
    TEST_ASSERT_EQUAL_FLOAT(3.0f, g_last_var_value);
}

void test_variable_change_rejects_bad_shape_directly(void)
{
    TEST_ASSERT_EQUAL_INT(TEC_ERR_NULL_ARG, tec_hk_variable_change(NULL, 6U));
    TEST_ASSERT_EQUAL_INT(TEC_ERR_BAD_LENGTH,
                          tec_hk_variable_change(VAR_CHANGE_OK, 5U));
    TEST_ASSERT_EQUAL_INT(TEC_ERR_BAD_LENGTH,
                          tec_hk_variable_change(VAR_CHANGE_OK, 0U));
    TEST_ASSERT_EQUAL_INT(0, g_var_write_calls);
}

/* Without the built-in handler registered, 0x03 is a defined task with no
 * binding — reported, not ignored. */
void test_variable_change_needs_registered_handler(void)
{
    TEST_ASSERT_EQUAL_INT(TEC_ERR_NO_HANDLER,
                          tec_dispatch(TEC_TYPE_HK, 0x03U,
                                       VAR_CHANGE_OK, sizeof VAR_CHANGE_OK));
    TEST_ASSERT_EQUAL_INT(0, g_var_write_calls);
}
