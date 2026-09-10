/* ---------------------------------------------------------------------------
 * bms_stubs.c — host double for App/bms/bms.c (EPS subsystem SPI link).
 *
 * WHY A DOUBLE AND NOT THE REAL MODULE: bms.c brings up SPI2 through the full
 * CubeMX SPI_HandleTypeDef layout (Instance, Init.Mode, Init.BaudRatePrescaler,
 * ...). The host fakes expose that struct as an opaque one-word placeholder on
 * purpose (see fakes/main.h) so the fake can never silently drift from the
 * real CubeMX definition. Linking bms.c on the host would require mirroring
 * the whole layout, so the module stays flight-only and the host build links
 * this double instead — exactly the trade already made for the radio
 * (support/radiolib_stubs.c).
 *
 * WHAT IT MODELS, AND WHY IT DEFAULTS TO FAILURE: the interesting contract is
 * NOT "reads a battery voltage". It is the failure semantics of the link:
 * `bms_poll()` returns non-zero and `bms_get_status()` reports valid == false
 * until a test says otherwise, so the DEFAULT host behaviour matches the
 * flight default (no EPS telemetry yet). Tests that assert the SoC-gated
 * paths therefore have to arm a reading explicitly, which is what makes
 * "unknown SoC closes the gates" testable instead of assumed.
 *
 * Test control API:
 *   host_bms_disarm()          -> back to "no telemetry" (poll fails)
 *   host_bms_arm(soc, valid)   -> poll succeeds and reports this snapshot
 * ------------------------------------------------------------------------- */
#include "bms.h"

#include <stddef.h>

static bms_status_t host_bms_status = {
    .soc        = 0,
    .temp_c     = 250,
    .voltage_mv = 7400,
    .valid      = false,
};

/* Non-zero = the poll fails. Default matches flight: no EPS frame format yet,
 * so no telemetry is ever produced. */
static int host_bms_poll_rc = -1;

void bms_init(void)
{
    /* Nothing to bring up on the host. */
}

bool bms_spi_ready(void)
{
    return (host_bms_poll_rc == 0);
}

bms_status_t bms_get_status(void)
{
    return host_bms_status;
}

int bms_poll(void)
{
    return host_bms_poll_rc;
}

/* ---------- test control ---------- */

void host_bms_disarm(void)
{
    host_bms_status.valid = false;
    host_bms_poll_rc      = -1;
}

void host_bms_arm(uint8_t soc, bool valid)
{
    host_bms_status.soc   = soc;
    host_bms_status.valid = valid;
    host_bms_poll_rc      = 0;
}
