/**
 * @file    test_comms_auth_provisioned.c
 * @brief   Uplink key provisioned at build time (COMMS_AUTH_KEY0..3 set in
 *          project.yml, as `make ... COMMS_AUTH_KEY=5A17C03E` does): frames
 *          sealed with that key dispatch, frames sealed with the published
 *          default key do not.
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

void test_provisioned_key_is_reported_usable(void)
{
    TEST_ASSERT_EQUAL_INT(1, COMMS_AUTH_KEY_PROVISIONED);
    TEST_ASSERT_TRUE(COMMS_AUTH_KEY_USABLE);
}

void test_frame_sealed_with_the_provisioned_key_dispatches(void)
{
    static const uint8_t key[4] = { 0x5AU, 0x17U, 0xC0U, 0x3EU };
    size_t n = seal(key, COMMS_TC_EXIT_STATE);

    state_machine_request_transition_ExpectAndReturn(STATE_READY, TRIGGER_GROUND_CMD, 0);
    TEST_ASSERT_EQUAL_INT(COMMS_TC_OK, comms_rx_handle_frame(frame_buf, n));
}

void test_frame_sealed_with_the_public_key_is_refused(void)
{
    static const uint8_t public_key[4] = { 0xA1U, 0xB2U, 0xC3U, 0xD4U };
    size_t n = seal(public_key, COMMS_TC_EXIT_STATE);

    TEST_ASSERT_EQUAL_INT(COMMS_TC_ERR_MAC, comms_rx_handle_frame(frame_buf, n));
}
