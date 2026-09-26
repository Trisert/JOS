/**
 * @file    test_comms_auth_key.c
 * @brief   Flight default of the uplink key (no COMMS_AUTH_KEY provisioned,
 *          no COMMS_AUTH_ALLOW_PUBLIC_KEY opt-in - see project.yml).
 *
 * The upstream default key A1 B2 C3 D4 is published in this repository, so a
 * tag made with it authenticates nobody. Before the fix the flight image
 * accepted exactly that key: anybody could seal COMMS_TC_RESET. Now an image
 * without a provisioned key refuses every authenticated frame (fail closed).
 */

#include "unity.h"
#include "comms.h"
#include "comms_validate.h"
#include "tec.h"
#include "sha256.h"
#include "mock_state_machine.h"
#include "mock_watchdog.h"     /* link seam only */
#include "mock_cmsis_os.h"     /* link seam only */

#include <string.h>

static uint8_t frame_buf[COMMS_TC_MAX_FRAME + 8];

/* "opcode | len | payload | TAG32-BE | CRC16-BE" sealed with `key`. */
static size_t seal(const uint8_t key[4], uint8_t opcode)
{
    uint8_t  mac[32];
    size_t   n = 0U;

    frame_buf[n++] = opcode;
    frame_buf[n++] = 0U;
    hmac_sha256(key, 4U, frame_buf, n, mac);
    memcpy(&frame_buf[n], mac, 4U);
    n += 4U;
    const uint16_t crc = comms_crc16_ccitt(frame_buf, n);
    frame_buf[n++] = (uint8_t)(crc >> 8);
    frame_buf[n++] = (uint8_t)(crc & 0xFFU);
    return n;
}

void setUp(void)    { memset(frame_buf, 0, sizeof(frame_buf)); }
void tearDown(void) { }

void test_image_without_a_provisioned_key_reports_it(void)
{
    TEST_ASSERT_EQUAL_INT(0, COMMS_AUTH_KEY_PROVISIONED);
    TEST_ASSERT_EQUAL_INT(0, COMMS_AUTH_ALLOW_PUBLIC_KEY);
    TEST_ASSERT_FALSE(COMMS_AUTH_KEY_USABLE);
}

/* A frame correctly sealed with the PUBLIC key is refused and never
 * dispatched (no state_machine_* expectation is queued: a dispatch would
 * fail the test through CMock). */
void test_frame_sealed_with_the_public_key_is_refused(void)
{
    static const uint8_t public_key[4] = { 0xA1U, 0xB2U, 0xC3U, 0xD4U };
    comms_rx_stats_t before, after;
    size_t n = seal(public_key, COMMS_TC_EXIT_STATE);

    comms_rx_get_stats(&before);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_MAC, comms_rx_handle_frame(frame_buf, n));
    comms_rx_get_stats(&after);
    TEST_ASSERT_EQUAL_UINT32(before.rejected_mac + 1U, after.rejected_mac);
    TEST_ASSERT_EQUAL_UINT32(before.accepted, after.accepted);
}
