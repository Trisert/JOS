#include "comms.h"
#include "comms_validate.h"
#include "state_machine.h"
#include "watchdog.h"
#include "cmsis_os.h"
#include "main.h"
#include "sram2_parity.h"   /* SRAM2_CRITICAL_NOINIT placement (W2-3) */
#include <string.h>

/* The RadioLib SX1268 driver lives in App/comms/radiolib_driver.cpp (wired in
   PR #47): lora_*() entry points are declared below with C linkage. */

/* Packet buffer sizes now live in comms.h as COMMS_MAX_PACKET /
   COMMS_BEACON_SIZE — the parity-protected SRAM2 buffers below are sized
   from them, and callers need the same sizes. */

extern SPI_HandleTypeDef hspi1;

/* RadioLib SX1268 driver entry points (C linkage, implemented in
   App/comms/radiolib_driver.cpp). Declared here so comms.c (compiled as C)
   links against the unmangled symbols. */
extern int  lora_init(void);
extern int  lora_tx(const uint8_t *data, size_t len);
extern int  lora_rx(uint8_t *data, size_t *len);
extern int  lora_tx_wait_done(uint32_t timeout_ms);
extern int  lora_start_receive(void);
/* Registers the RX task handle with the driver so the DIO1 ISR wakes this task
   on RX_DONE. Implemented in radiolib_driver.cpp. */
extern void lora_rx_task_register(osThreadId_t handle);

/* LORA_RX_FLAG is defined in comms.h (must match LORA_FLAG_RX_DONE in
   radiolib_driver.cpp — see comment there). */

/* ---------- Packet buffers in SRAM2 (hardware parity) ----------
   Placed in the NOLOAD .sram2_noinit section: the SRAM2 hardware erase run by
   sram2_parity_init() zeroes them at boot with valid parity, so they cost no
   Flash. A single-event upset in a frame being assembled or decoded now
   raises an NMI (recorded + reset) instead of transmitting or executing
   corrupted data. */
static SRAM2_CRITICAL_NOINIT uint8_t comms_beacon_buf[COMMS_BEACON_SIZE];
static SRAM2_CRITICAL_NOINIT uint8_t comms_rx_buf[COMMS_MAX_PACKET];
static SRAM2_CRITICAL_NOINIT uint8_t comms_tx_buf[COMMS_MAX_PACKET];

uint8_t *comms_beacon_buffer(size_t *len)
{
    if (len != NULL) { *len = sizeof(comms_beacon_buf); }
    return comms_beacon_buf;
}

uint8_t *comms_rx_buffer(size_t *len)
{
    if (len != NULL) { *len = sizeof(comms_rx_buf); }
    return comms_rx_buf;
}

uint8_t *comms_tx_buffer(size_t *len)
{
    if (len != NULL) { *len = sizeof(comms_tx_buf); }
    return comms_tx_buf;
}

/* ---------- Stub implementations ---------- */

/* lora_init() is implemented in radiolib_driver.cpp (target build). The host
   unit-test build links a fake from test/fakes/ instead (see B4). */

/* TX sequence counters (defined below; single writer — see note there). */
static comms_tx_stats_t tx_stats;

int lora_send_chunked(const uint8_t *data, size_t len)
{
    size_t chunk_max = 0U;
    uint8_t *tx = comms_tx_buffer(&chunk_max);

    if ((data == NULL) && (len != 0U)) {
        return -1;
    }
    if (len == 0U) {
        return 0;   /* nothing to stage — NULL/0 is a successful no-op */
    }
    if (chunk_max <= (size_t)COMMS_CHUNK_HDR_LEN) {
        return -1;  /* TX buffer cannot even hold the framing header */
    }

    /* Payload bytes per chunk after the msg + seq + total header. */
    const size_t payload_max = chunk_max - (size_t)COMMS_CHUNK_HDR_LEN;

    /* Bounded: payload_max >= 1, so total >= 1 and <= len. */
    const size_t total = ((len - 1U) / payload_max) + 1U;
    if (total > 255U) {
        return -1;  /* count must fit in the 1-byte total field */
    }

    /* Monotonic message id: consecutive sequences carry consecutive ids so
       ground can distinguish beacons and flag duplicates. Wraps mod 256;
       only consumed here (single writer — the beacon path is one task). */
    static uint8_t tx_msg_id = 0U;
    const uint8_t msg = tx_msg_id++;

    /* Hand each framed chunk to RadioLib. TX is async (startTransmit); wait
       for the DIO1 TX_DONE flag before staging the next chunk so we never
       overwrite the buffer mid-air. 2000 ms covers SF10 @ 125 kHz for the
       largest chunk.
       On ANY radio failure the sequence aborts here: no further chunks go on
       air (the partial message is detectable via its total field), the
       failure is counted, and the caller retries the whole message later. */
    for (size_t seq = 0U; seq < total; seq++) {
        const size_t off = seq * payload_max;   /* no overflow: seq < total <= len */
        size_t n = len - off;
        if (n > payload_max) {
            n = payload_max;
        }
        tx[COMMS_CHUNK_OFF_MSG]   = msg;
        tx[COMMS_CHUNK_OFF_SEQ]   = (uint8_t)seq;
        tx[COMMS_CHUNK_OFF_TOTAL] = (uint8_t)total;
        memcpy(&tx[COMMS_CHUNK_HDR_LEN], data + off, n);
        if (lora_tx(tx, n + (size_t)COMMS_CHUNK_HDR_LEN) != 0) {
            tx_stats.sequences_failed++;
            return -1;
        }
        tx_stats.chunks_sent++;
        if (lora_tx_wait_done(2000U) != 0) {
            tx_stats.sequences_failed++;
            return -1;
        }
    }
    tx_stats.sequences_ok++;
    return 0;
}

/* ---------- TX counters ---------- */

/* TX counters live at file scope above (single writer — lora_send_chunked()
 * runs only on the beacon task, so the increments never race). Same snapshot
 * semantics as the RX counters in comms_validate.c (diagnostics-grade, not
 * flight-critical). */
void comms_tx_get_stats(comms_tx_stats_t *out)
{
    if (out != NULL) {
        out->sequences_ok     = tx_stats.sequences_ok;
        out->sequences_failed = tx_stats.sequences_failed;
        out->chunks_sent      = tx_stats.chunks_sent;
    }
}

/* Compile-time proof for the chunk_max <= HDR guard above: with the fixed
   SRAM2 sizing the TX buffer always holds a header plus payload. If someone
   ever shrinks COMMS_MAX_PACKET to the header size, this — not a runtime
   field failure — tells them. */
_Static_assert(COMMS_MAX_PACKET > COMMS_CHUNK_HDR_LEN,
               "TX buffer must hold the chunk header plus payload");

/* ======================================================================
 * TT&C command frame codec (see comms.h for the layout and the MAC seam)
 * ====================================================================== */

/* MAC verification hook. NULL = semantics undefined => fail closed: the RX
 * path parses the frame but never dispatches it. See docs/api/ttc-frame.md. */
static comms_ttc_mac_verify_fn ttc_mac_verifier = NULL;

/* De-interleaved header + payload of the last parsed ECC-ON frame (an ECC-ON
 * packet interleaves 10 data with 6 parity bytes per block, so those fields
 * are not contiguous on air). Sized for the largest legal content — header
 * plus the 100-byte maximum payload. Only ever written/read by
 * comms_ttc_parse_frame(); the RX path is single-threaded. */
static uint8_t ttc_content_scratch[(size_t)COMMS_TTC_HDR_LEN +
                                   (size_t)COMMS_TTC_MAX_PL];

void comms_ttc_set_mac_verifier(comms_ttc_mac_verify_fn fn)
{
    ttc_mac_verifier = fn;
}

bool comms_ttc_mac_verify(const uint8_t *frame, size_t len,
                          const uint8_t mac[4])
{
    if (ttc_mac_verifier == NULL) {
        return false;   /* no definition installed -> reject (fail closed) */
    }
    return ttc_mac_verifier(frame, len, mac);
}

/* ---------- 16-byte block geometry (ECC OFF vs ECC ON) ----------
 *
 * 'Packet structure'!A14 requires a packet that is 16*n long ("for correct
 * interleaving") and !H54 fixes the RS PARITY element at 6 bytes. Both hold
 * together when the parity lives INSIDE each 16-byte block:
 *   ECC OFF -> the block is 16 data bytes;
 *   ECC ON  -> the block is a systematic RS(16,10) codeword = 10 data + 6
 *              parity bytes.
 * An ECC packet is therefore 16*ceil(content/10), NOT 16*n + 6, and both
 * geometries are trivially 16-aligned. Assumption A1 in docs/api/ttc-frame.md
 * records the ordering choice (systematic, 10 data then 6 parity) against the
 * sheet's own sparse-parity sketch, which does not sum to 16 uniquely.
 */
_Static_assert((COMMS_TTC_RS_DATA_LEN + COMMS_TTC_RS_PARITY_LEN) == COMMS_TTC_BLOCK,
               "one RS codeword must fill exactly one 16-byte interleaving block");

/* Data bytes carried by a frame of @p total_len (the codeword data slots). */
static size_t ttc_data_capacity(size_t total_len, bool ecc_on)
{
    if (!ecc_on) {
        return total_len;
    }
    return (total_len / (size_t)COMMS_TTC_BLOCK) * (size_t)COMMS_TTC_RS_DATA_LEN;
}

/* On-air offset of the frame content byte at index @p i. With ECC off the
 * content is laid out verbatim; with ECC on the byte sits in block
 * (i / 10), slot (i % 10), and the 6 parity slots that follow every 10-byte
 * data field are skipped. Indices 0..9 map to themselves in both modes, so
 * the INFO field stays readable at its fixed offsets (the layout
 * discriminator relies on byte 1 being the ECC flag). */
static size_t ttc_onair_off(size_t i, bool ecc_on)
{
    if (!ecc_on) {
        return i;
    }
    return ((i / (size_t)COMMS_TTC_RS_DATA_LEN) * (size_t)COMMS_TTC_BLOCK) +
           (i % (size_t)COMMS_TTC_RS_DATA_LEN);
}

size_t comms_ttc_padded_len(size_t content_len, bool ecc_on)
{
    if (content_len == 0U) {
        return 0U;
    }
    const size_t per_block = ecc_on ? (size_t)COMMS_TTC_RS_DATA_LEN
                                    : (size_t)COMMS_TTC_BLOCK;
    const size_t blocks    = (content_len + (per_block - 1U)) / per_block;
    return blocks * (size_t)COMMS_TTC_BLOCK;
}

/* ---------- Reed-Solomon parity (RS PARITY element, 6 bytes per block) ----
 *
 * Systematic RS(255,249) shortened code over GF(256), used to fill the 6
 * parity slots of every 16-byte block of an ECC-enabled command packet.
 * Field/generator citation is on the
 * comms_ttc_rs_ecc_encode() prototype in comms.h (primitive polynomial
 * 0x11D = x^8+x^4+x^3+x^2+1, alpha = 2, g(x) = prod (x - alpha^i)) — the
 * standard RS(255,249) parameters, not invented here. Kept in this TU rather
 * than a separate rs_ecc.c because Ceedling links each test binary from the
 * headers that the test includes: a separate module's .c would not be linked
 * into test_comms_legacy.c (which does not include it), leaving an undefined
 * reference there.
 */

/* Carry-less multiply in GF(256) reduced modulo 0x11D. No lookup tables:
   fixed 8-step loop, no RAM init-order dependency. */
static uint8_t ttc_gf_mul(uint8_t a, uint8_t b)
{
    uint8_t p = 0U;

    for (uint8_t i = 0U; i < 8U; i++) {
        if ((b & 1U) != 0U) {
            p ^= a;
        }
        const uint8_t hi = (uint8_t)(a & 0x80U);
        a = (uint8_t)(a << 1);
        if (hi != 0U) {
            a ^= 0x1DU;   /* x^8 + x^4 + x^3 + x^2 + 1, folded back */
        }
        b = (uint8_t)(b >> 1);
    }
    return p;
}

/* alpha^n in GF(256), alpha = 2. n is always < COMMS_TTC_RS_NSYM. */
static uint8_t ttc_gf_alpha_pow(unsigned n)
{
    uint8_t v = 1U;

    for (unsigned i = 0U; i < (n % 255U); i++) {
        v = ttc_gf_mul(v, 2U);
    }
    return v;
}

void comms_ttc_rs_ecc_encode(const uint8_t *data, size_t len,
                             uint8_t parity[COMMS_TTC_RS_NSYM])
{
    uint8_t gen[COMMS_TTC_RS_NSYM + 1U] = { 0 };
    uint8_t rem[COMMS_TTC_RS_NSYM]      = { 0 };

    if (parity == NULL) {
        return;
    }
    memset(parity, 0, COMMS_TTC_RS_NSYM);
    if ((data == NULL) || (len > (size_t)COMMS_TTC_RS_MAX_DATA)) {
        return;   /* leaves parity zeroed — never partially written */
    }

    /* g(x) = prod_{i=0}^{nsym-1} (x - alpha^i); monic, degree NSYM. gen[0]
       is the leading 1, gen[1..NSYM] the lower-degree coefficients (so the
       LFSR below can divide by it in place). */
    gen[0] = 1U;
    for (size_t i = 0U; i < (size_t)COMMS_TTC_RS_NSYM; i++) {
        const uint8_t root = ttc_gf_alpha_pow((unsigned)i);
        gen[i + 1U] = 0U;
        for (size_t j = i + 1U; j > 0U; j--) {
            gen[j] = (uint8_t)(gen[j] ^ ttc_gf_mul(gen[j - 1U], root));
        }
    }

    /* Systematic division: shift the message through the NSYM-cell LFSR. */
    for (size_t i = 0U; i < len; i++) {
        const uint8_t factor = (uint8_t)(data[i] ^ rem[0]);
        for (size_t j = 0U; (j + 1U) < (size_t)COMMS_TTC_RS_NSYM; j++) {
            rem[j] = (uint8_t)(rem[j + 1U] ^ ttc_gf_mul(gen[j + 1U], factor));
        }
        rem[COMMS_TTC_RS_NSYM - 1U] = ttc_gf_mul(gen[COMMS_TTC_RS_NSYM], factor);
    }

    memcpy(parity, rem, COMMS_TTC_RS_NSYM);
}

uint8_t comms_ttc_info_pack(uint8_t tec_type, uint8_t tec_task)
{
    return (uint8_t)(((tec_type << 6) & COMMS_TTC_TEC_TYPE_MASK) |
                     (tec_task & COMMS_TTC_TEC_TASK_MASK));
}

void comms_ttc_info_unpack(uint8_t byte2, uint8_t *tec_type, uint8_t *tec_task)
{
    if (tec_type != NULL) {
        *tec_type = (uint8_t)((byte2 & COMMS_TTC_TEC_TYPE_MASK) >> 6);
    }
    if (tec_task != NULL) {
        *tec_task = (uint8_t)(byte2 & COMMS_TTC_TEC_TASK_MASK);
    }
}

comms_ttc_result_t comms_ttc_build_frame(uint8_t                *out,
                                         size_t                  cap,
                                         const comms_ttc_info_t *info,
                                         uint32_t                unix_time,
                                         const uint8_t          *mac,
                                         const uint8_t          *payload,
                                         size_t                  pl_len,
                                         size_t                 *out_len)
{
    if ((out == NULL) || (info == NULL)) {
        return COMMS_TTC_ERR_NULL;
    }
    if ((payload == NULL) && (pl_len != 0U)) {
        return COMMS_TTC_ERR_NULL;
    }
    if (pl_len > (size_t)COMMS_TTC_MAX_PL) {
        return COMMS_TTC_ERR_PL_LEN;
    }
    if ((size_t)info->pl_len != pl_len) {
        return COMMS_TTC_ERR_PL_LEN;   /* INFO must agree with the payload */
    }
    if (info->station_id < COMMS_TTC_STATION_MIN) {
        return COMMS_TTC_ERR_STATION_ID;
    }
    if ((info->ecc_flag != COMMS_TTC_ECC_OFF) &&
        (info->ecc_flag != COMMS_TTC_ECC_ON)) {
        return COMMS_TTC_ERR_ECC_FLAG;
    }
    if (info->tec_type > COMMS_TTC_TEC_TYPE_MAX) {
        return COMMS_TTC_ERR_TEC_TYPE;
    }
    if (info->tec_task > COMMS_TTC_TEC_TASK_MAX) {
        return COMMS_TTC_ERR_TEC_TASK;
    }

    const bool   ecc_on  = (info->ecc_flag == COMMS_TTC_ECC_ON);
    const size_t content = (size_t)COMMS_TTC_HDR_LEN + pl_len;
    const size_t total   = comms_ttc_padded_len(content, ecc_on);

    if (out_len != NULL) {
        *out_len = total;   /* filled even on ERR_BUF, to size a retry */
    }
    if (cap < total) {
        return COMMS_TTC_ERR_BUF;
    }

    /* Zero the whole packet up front: that is the mandated zero padding
     * ('Packet structure'!J44/J50) AND it clears the 6 parity slots of every
     * block before the encoder fills them. */
    memset(out, 0, total);

    /* Content bytes, placed through the block mapping: with ECC off they land
     * at their natural offsets, with ECC on at block (i/10), slot (i%10). */
    out[ttc_onair_off(0U, ecc_on)] = info->station_id;
    out[ttc_onair_off(1U, ecc_on)] = info->ecc_flag;
    out[ttc_onair_off(2U, ecc_on)] = comms_ttc_info_pack(info->tec_type,
                                                         info->tec_task);
    out[ttc_onair_off(3U, ecc_on)] = (uint8_t)pl_len;
    out[ttc_onair_off(COMMS_TTC_INFO_LEN, ecc_on)]      = (uint8_t)(unix_time >> 24);
    out[ttc_onair_off(COMMS_TTC_INFO_LEN + 1U, ecc_on)] = (uint8_t)(unix_time >> 16);
    out[ttc_onair_off(COMMS_TTC_INFO_LEN + 2U, ecc_on)] = (uint8_t)(unix_time >> 8);
    out[ttc_onair_off(COMMS_TTC_INFO_LEN + 3U, ecc_on)] = (uint8_t)unix_time;
    for (size_t i = 0U; i < (size_t)COMMS_TTC_MAC_LEN; i++) {
        out[ttc_onair_off((size_t)COMMS_TTC_INFO_LEN + COMMS_TTC_TIME_LEN + i,
                          ecc_on)] = (mac != NULL) ? mac[i] : 0U;
    }
    for (size_t i = 0U; i < pl_len; i++) {
        out[ttc_onair_off((size_t)COMMS_TTC_HDR_LEN + i, ecc_on)] = payload[i];
    }

    if (ecc_on) {
        /* One systematic RS(16,10) codeword per block: the 6 parity symbols
         * are computed over the block's 10 data bytes (the zero padding of the
         * last block included) and written into its parity slots — never left
         * zeroed. */
        const size_t blocks = total / (size_t)COMMS_TTC_BLOCK;
        for (size_t b = 0U; b < blocks; b++) {
            uint8_t *blk = &out[b * (size_t)COMMS_TTC_BLOCK];
            comms_ttc_rs_ecc_encode(blk, (size_t)COMMS_TTC_RS_DATA_LEN,
                                    &blk[COMMS_TTC_RS_DATA_LEN]);
        }
    }
    return COMMS_TTC_OK;
}

comms_ttc_result_t comms_ttc_parse_frame(const uint8_t     *frame,
                                         size_t             len,
                                         comms_ttc_frame_t *out)
{
    if ((frame == NULL) || (out == NULL)) {
        return COMMS_TTC_ERR_NULL;
    }
    if (len < (size_t)COMMS_TTC_MIN_FRAME) {
        return COMMS_TTC_ERR_TOO_SHORT;
    }
    if (len > (size_t)COMMS_TTC_MAX_FRAME) {
        return COMMS_TTC_ERR_TOO_LONG;
    }

    const uint8_t station = frame[0];
    const uint8_t ecc     = frame[1];
    const uint8_t pl_len  = frame[3];
    uint8_t       tec_type = 0U;
    uint8_t       tec_task = 0U;

    if (station < COMMS_TTC_STATION_MIN) {
        return COMMS_TTC_ERR_STATION_ID;
    }
    if ((ecc != COMMS_TTC_ECC_OFF) && (ecc != COMMS_TTC_ECC_ON)) {
        return COMMS_TTC_ERR_ECC_FLAG;
    }
    comms_ttc_info_unpack(frame[2], &tec_type, &tec_task);
    /* No TEC type/task range check here: comms_ttc_info_unpack() masks the
     * fields to their widths (type 2 bits -> 0..3, task 6 bits -> 0..63), so
     * an out-of-range value cannot be produced from the wire. The checks in
     * comms_ttc_build_frame() guard the caller-supplied struct instead. */
    if (pl_len > COMMS_TTC_MAX_PL) {
        return COMMS_TTC_ERR_PL_LEN;
    }

    const bool   ecc_on = (ecc == COMMS_TTC_ECC_ON);
    /* Both geometries are 16*n in full (the RS parity lives inside the blocks,
       see the block-geometry note above), so the alignment test is
       unconditional and the ECC flag changes only the DATA capacity. */
    if ((len % (size_t)COMMS_TTC_BLOCK) != 0U) {
        return COMMS_TTC_ERR_ALIGN;      /* not a whole number of blocks */
    }
    /* Canonical length: exactly one frame length is legal for a given
     * (PL length, ECC flag) pair. Rejecting anything else (e.g. len==32 on
     * an ECC-off header-only frame) removes length-smuggling room. Checked
     * BEFORE the header is de-interleaved, so a short frame can never make
     * the block mapping read out of bounds. */
    if (len != comms_ttc_padded_len((size_t)COMMS_TTC_HDR_LEN + pl_len, ecc_on)) {
        return COMMS_TTC_ERR_LEN_MISMATCH;
    }
    const size_t content  = (size_t)COMMS_TTC_HDR_LEN + pl_len;
    const size_t data_len = ttc_data_capacity(len, ecc_on);
    if (content > data_len) {
        return COMMS_TTC_ERR_PL_LEN;     /* payload truncated by the padding */
    }

    out->info.station_id = station;
    out->info.ecc_flag   = ecc;
    out->info.tec_type   = tec_type;
    out->info.tec_task   = tec_task;
    out->info.pl_len     = pl_len;
    out->unix_time = ((uint32_t)frame[ttc_onair_off(COMMS_TTC_INFO_LEN, ecc_on)] << 24) |
                     ((uint32_t)frame[ttc_onair_off(COMMS_TTC_INFO_LEN + 1U, ecc_on)] << 16) |
                     ((uint32_t)frame[ttc_onair_off(COMMS_TTC_INFO_LEN + 2U, ecc_on)] << 8) |
                      (uint32_t)frame[ttc_onair_off(COMMS_TTC_INFO_LEN + 3U, ecc_on)];
    if (ecc_on) {
        /* De-interleave the header + payload out of the 10+6 blocks (they are
           not contiguous on air). Valid until the next parse call. */
        for (size_t i = 0U; i < content; i++) {
            ttc_content_scratch[i] = frame[ttc_onair_off(i, true)];
        }
        out->mac     = &ttc_content_scratch[COMMS_TTC_INFO_LEN + COMMS_TTC_TIME_LEN];
        out->payload = (pl_len > 0U) ? &ttc_content_scratch[COMMS_TTC_HDR_LEN]
                                     : NULL;
    } else {
        out->mac     = &frame[COMMS_TTC_INFO_LEN + COMMS_TTC_TIME_LEN];
        out->payload = (pl_len > 0U) ? &frame[COMMS_TTC_HDR_LEN] : NULL;
    }
    out->frame_len = len;
    out->data_len  = data_len;
    return COMMS_TTC_OK;
}

const char *comms_ttc_result_str(comms_ttc_result_t result)
{
    switch (result) {
    case COMMS_TTC_OK:              return "OK";
    case COMMS_TTC_ERR_NULL:        return "NULL";
    case COMMS_TTC_ERR_TOO_SHORT:   return "TOO_SHORT";
    case COMMS_TTC_ERR_TOO_LONG:    return "TOO_LONG";
    case COMMS_TTC_ERR_ALIGN:       return "ALIGN";
    case COMMS_TTC_ERR_LEN_MISMATCH: return "LEN_MISMATCH";
    case COMMS_TTC_ERR_PL_LEN:      return "PL_LEN";
    case COMMS_TTC_ERR_STATION_ID:  return "STATION_ID";
    case COMMS_TTC_ERR_ECC_FLAG:    return "ECC_FLAG";
    case COMMS_TTC_ERR_TEC_TYPE:    return "TEC_TYPE";
    case COMMS_TTC_ERR_TEC_TASK:    return "TEC_TASK";
    case COMMS_TTC_ERR_UNSUPPORTED: return "UNSUPPORTED";
    case COMMS_TTC_ERR_PAYLOAD:     return "PAYLOAD";
    case COMMS_TTC_ERR_MAC:         return "MAC";
    case COMMS_TTC_ERR_BUF:         return "BUF";
    default:                        return "UNKNOWN";
    }
}

comms_tc_result_t comms_ttc_to_tc_result(comms_ttc_result_t result)
{
    switch (result) {
    case COMMS_TTC_OK:             return COMMS_TC_OK;
    case COMMS_TTC_ERR_NULL:       return COMMS_TC_ERR_NULL;
    case COMMS_TTC_ERR_TOO_SHORT:  return COMMS_TC_ERR_TOO_SHORT;
    case COMMS_TTC_ERR_TOO_LONG:   return COMMS_TC_ERR_TOO_LONG;
    case COMMS_TTC_ERR_PL_LEN:     return COMMS_TC_ERR_PAYLOAD_LEN;
    case COMMS_TTC_ERR_PAYLOAD:    return COMMS_TC_ERR_PAYLOAD_LEN;
    case COMMS_TTC_ERR_STATION_ID:
    case COMMS_TTC_ERR_ECC_FLAG:
    case COMMS_TTC_ERR_TEC_TYPE:
    case COMMS_TTC_ERR_TEC_TASK:   return COMMS_TC_ERR_PARAM_RANGE;
    case COMMS_TTC_ERR_UNSUPPORTED: return COMMS_TC_ERR_OPCODE;
    case COMMS_TTC_ERR_MAC:        return COMMS_TC_ERR_MAC;
    case COMMS_TTC_ERR_ALIGN:
    case COMMS_TTC_ERR_LEN_MISMATCH:
    case COMMS_TTC_ERR_BUF:
    default:                       return COMMS_TC_ERR_LEN_MISMATCH;
    }
}

bool comms_frame_is_ttc_layout(const uint8_t *frame, size_t len)
{
    if (frame == NULL) {
        return false;
    }
    if ((len < (size_t)COMMS_TTC_MIN_FRAME) || (len > (size_t)COMMS_TTC_MAX_FRAME)) {
        return false;
    }
    /* INFO byte 1 = ECC flag (0x55/0xAA), at the same offset with ECC on and
     * off (content indices 0..9 map to themselves in both geometries). A
     * legacy/auth frame carries its payload length there (0..60), so the two
     * can never collide. Both TT&C geometries are a whole number of 16-byte
     * interleaving blocks — the parity sits inside them, no tail. */
    if ((frame[1] == COMMS_TTC_ECC_OFF) || (frame[1] == COMMS_TTC_ECC_ON)) {
        return (len % COMMS_TTC_BLOCK) == 0U;
    }
    return false;
}

/* Deliver an already-parsed, already-authenticated TT&C command by TEC type
 * and task. File-static: the only legal caller is comms_rx_handle_ttc_frame()
 * after the MAC seam has accepted the frame.
 *
 * Returns COMMS_TTC_OK only when the command was actually executed/accepted;
 * COMMS_TTC_ERR_UNSUPPORTED for a TEC type/task this build does not implement
 * (or a non-HK type, which is not a command carrier) and
 * COMMS_TTC_ERR_PAYLOAD for a command whose payload is short or incoherent.
 * The RX entry point accounts the returned verdict, so neither class is ever
 * counted as accepted (CodeRabbit orange, comms.c:412). */
static comms_ttc_result_t comms_ttc_dispatch_unchecked(const comms_ttc_frame_t *f)
{
    if (f->info.tec_type != COMMS_TTC_TEC_HK) {
        /* DAQ / PE / DT are not command carriers in the source's HK table. */
        return COMMS_TTC_ERR_UNSUPPORTED;
    }
    switch (f->info.tec_task) {
    case COMMS_TTC_TASK_OBC_REBOOT:
        if (f->info.pl_len != 0U) {
            return COMMS_TTC_ERR_PAYLOAD;   /* 'Task details'!G5: PL = 0 */
        }
        NVIC_SystemReset();
        break;
    case COMMS_TTC_TASK_EXIT_STATE: {
        /* 'HK tasks'!D13:D14 + G6: payload = 1 byte old state, 1 byte new
         * state (PL = 2). Apply the REQUESTED new state; the empty-payload
         * path is rejected instead of silently forcing STATE_READY. */
        if (f->info.pl_len != 2U) {
            return COMMS_TTC_ERR_PAYLOAD;
        }
        const uint8_t old_state = f->payload[0];
        const uint8_t new_state = f->payload[1];
        if ((old_state >= (uint8_t)OBW_STATE_COUNT) ||
            (new_state >= (uint8_t)OBW_STATE_COUNT) ||
            (old_state == new_state)) {
            return COMMS_TTC_ERR_PAYLOAD;   /* out of range / no-op request */
        }
        (void)state_machine_request_transition((obw_state_t)new_state,
                                               TRIGGER_GROUND_CMD);
        break;
    }
    default:
        /* TODO: remaining HK commands (variable change, set time, TLE,
         * EPS/ADCS reboot, LoRa state/config/ping, ACK/NACK). */
        return COMMS_TTC_ERR_UNSUPPORTED;
    }
    return COMMS_TTC_OK;
}

comms_tc_result_t comms_rx_handle_ttc_frame(const uint8_t *frame, size_t len)
{
    comms_ttc_frame_t  parsed;
    comms_ttc_result_t verdict = comms_ttc_parse_frame(frame, len, &parsed);

    if (verdict == COMMS_TTC_OK) {
        if (!comms_ttc_mac_verify(frame, len, parsed.mac)) {
            verdict = COMMS_TTC_ERR_MAC;   /* fail closed until MAC defined */
        }
    }
    if (verdict == COMMS_TTC_OK) {
        /* Unsupported commands and malformed payloads are rejected HERE, so
         * they are accounted as rejections — never as accepted. A command
         * that executes (OBC reboot resets immediately) returns no further. */
        verdict = comms_ttc_dispatch_unchecked(&parsed);
    }

    const comms_tc_result_t tc_result = comms_ttc_to_tc_result(verdict);
    comms_rx_account(tc_result);
    return tc_result;
}

/* ---------- Telecommand dispatcher (private) ---------- */

/**
 * Execute an already-validated telecommand.
 *
 * Deliberately file-static and suffixed @c _unchecked: it performs NO
 * structural validation of its own, so the only legal caller is
 * comms_rx_handle_frame(), which runs comms_validate_tc() first (length, CRC,
 * opcode whitelist, per-opcode payload size and parameter ranges).
 * Exporting it would make the validation gate bypassable.
 */
static void comms_dispatch_command_unchecked(uint8_t cmd_id,
                                             const uint8_t *payload,
                                             size_t len)
{
    switch (cmd_id) {
    case COMMS_TC_RESET:
        NVIC_SystemReset();
        break;
    case COMMS_TC_EXIT_STATE:
        state_machine_request_transition(STATE_READY, TRIGGER_GROUND_CMD);
        break;
    case COMMS_TC_SET_CONFIG:
        /* TODO: apply config from payload */
        break;
    case COMMS_TC_SEND_DATA:
        /* TODO: read FRAM and send chunked */
        break;
    case COMMS_TC_ACTIVATE_PAYLOAD:
        state_machine_request_transition(STATE_ACTIVE, TRIGGER_GROUND_CMD);
        break;
    case COMMS_TC_SET_BEACON_INTERVAL:
        if ((payload != NULL) && (len >= 4U)) {
            uint32_t interval_ms = ((uint32_t)payload[0] << 24) |
                                   ((uint32_t)payload[1] << 16) |
                                   ((uint32_t)payload[2] << 8)  |
                                    (uint32_t)payload[3];
            /* Defence in depth: comms_validate_tc() already range-checked this,
             * re-check here so a future in-file caller cannot bypass the
             * bounds. NASA-PoT #1 / NASA-STD-8739.8. */
            if ((interval_ms == 0UL) ||
                ((interval_ms >= COMMS_TC_BEACON_MIN_MS) &&
                 (interval_ms <= COMMS_TC_BEACON_MAX_MS))) {
                state_machine_set_beacon_interval(interval_ms);
            }
        }
        break;
    default:
        /* Unreachable via comms_rx_handle_frame(): unknown opcodes are
         * rejected by the whitelist. Kept as a defensive no-op. */
        break;
    }
}

/* ---------- Uplink validation gate ---------- */

/**
 * Validate a raw uplink frame and dispatch it only if it is well formed,
 * CRC-clean, HMAC-authenticated, of a known opcode and with in-range
 * parameters. Every rejection is counted (comms_rx_get_stats) and never
 * reaches the dispatcher.
 *
 * Scope: structural validation (framing, CRC-16 integrity, opcode whitelist,
 * parameter ranges) PLUS keyed authentication. The CRC is unkeyed, so it
 * detects corruption and malformed frames only; the truncated HMAC-SHA256 tag
 * (upstream RedPill makeMAC design) authenticates the sender. Layout
 * discrimination is by exact length: len == P+8 selects the authenticated
 * validator, anything else the legacy one. With COMMS_AUTH_ENFORCE=1 (flight
 * default) a structurally valid but untagged legacy frame is rejected with
 * COMMS_TC_ERR_MAC; with COMMS_AUTH_ENFORCE=0 (bench/migration only) legacy
 * CRC-only frames are still dispatched. Either way a bad tag is rejected
 * before the dispatcher runs (verify-then-dispatch).
 *
 * Standards: NASA-PoT #1 (bounds-checked, no overflow), NASA-STD-8739.8
 * (command validation before execution).
 */
comms_tc_result_t comms_rx_handle_frame(const uint8_t *frame, size_t len)
{
    uint8_t        opcode      = 0U;
    const uint8_t *payload     = NULL;
    size_t         payload_len = 0U;
    comms_tc_result_t result;

    if (comms_frame_is_ttc_layout(frame, len)) {
        /* Spec TT&C command frame — a different layout from opcode|len|... .
         * Parsed, MAC-seam checked and dispatched by TEC type/task; the
         * legacy/authenticated layouts below are never reached for it. */
        return comms_rx_handle_ttc_frame(frame, len);
    }

    if (comms_frame_is_auth_layout(frame, len)) {
        result = comms_validate_tc_auth(frame, len,
                                        &opcode, &payload, &payload_len);
    } else {
        result = comms_validate_tc(frame, len, &opcode, &payload, &payload_len);
#if COMMS_AUTH_ENFORCE
        /* Structurally valid but untagged: well-formed legacy frames parse
         * cleanly yet carry no authentication — reject, do NOT dispatch.
         * Malformed frames keep their structural verdict for telemetry. */
        if (result == COMMS_TC_OK) {
            result = COMMS_TC_ERR_MAC;
        }
#endif
    }

    comms_rx_account(result);

    if (result != COMMS_TC_OK) {
        return result;   /* rejected — do NOT dispatch */
    }

    comms_dispatch_command_unchecked(opcode, payload, payload_len);
    return COMMS_TC_OK;
}

/* ---------- Beacon TX task ---------- */

static const osThreadAttr_t beacon_attrs = {
    .name       = "loraBeacon",
    .stack_size = 256 * 4,
    .priority   = osPriorityBelowNormal,
};

void lora_beacon_task(void *arg)
{
    (void)arg;
    uint32_t registered_period_ms = 0u;

    for (;;) {
        uint32_t interval = state_machine_get_beacon_interval();

        /* The beacon cadence is state-dependent (1..16 min) and can also be
           retargeted from ground via CMD_SET_BEACON_INTERVAL. Registering the
           worst case once at creation would make the monitor blind for up to
           3 x 16 min even when the task is supposed to run every minute.
           Re-registering the *same* handle takes the duplicate-refresh path in
           watchdog_register_task(): the existing slot is updated in place with
           the new period and its last_tick is reset, so the monitor always
           tracks the cadence actually in force and the period change itself
           cannot false-flag the task. */
        if (interval != registered_period_ms) {
            if (watchdog_register_task(osThreadGetId(), interval) == 0) {
                registered_period_ms = interval;
            }
        }

        watchdog_alive_self();

        size_t beacon_len = 0U;
        const uint8_t *beacon = comms_beacon_buffer(&beacon_len);

        /* Build beacon packet (96 B telemetry + 32 B sys) in `beacon`.
           TODO: full telemetry encoding. For now transmit the staging buffer
           as-is so the link is exercised end-to-end. The 128 B beacon does
           NOT fit one LoRa payload (COMMS_MAX_PACKET = 64): fragment it via
           lora_send_chunked(), which frames every chunk with a msg + seq +
           total header (3 chunks on the air).
           A failed sequence is already counted in the TX stats by
           lora_send_chunked() — the whole message is retried here at the next
           interval, never resumed mid-sequence (ground detects the truncation
           via the total field).
           Blocking note: a full beacon holds this task for up to ~6 s
           (3 x 2 s TX_DONE waits). The watchdog judges this task against its
           1..16 min cadence and flags it only after 3x the period (>= 3 min),
           so the block is two orders of magnitude inside the monitor window. */
        if (beacon_len > 0U) {
            if (lora_send_chunked(beacon, beacon_len) != 0) {
                /* counted — retry the whole message next interval */
            }
        }

        osDelay(pdMS_TO_TICKS(interval));
    }
}

osThreadId_t lora_beacon_task_create(void)
{
    osThreadId_t handle = osThreadNew(lora_beacon_task, NULL, &beacon_attrs);
    if (handle != NULL) {
        /* Bootstrap with the slowest permitted cadence so the task is covered
           from the first tick; the task itself narrows the period to the
           cadence actually in force on its first iteration (duplicate-refresh). */
        (void)watchdog_register_task(handle, WDG_PERIOD_LORA_BEACON_MS);
    }
    return handle;
}

/* ---------- RX task ---------- */

static const osThreadAttr_t rx_attrs = {
    .name       = "loraRX",
    .stack_size = 256 * 4,
    .priority   = osPriorityNormal,
};

/*
 * NOW WIRED: the SX1268 driver (radiolib_driver.cpp) lands the PHY payload into
 * `rx` via the DIO1 IRQ. The ONLY permitted path from PHY payload to dispatcher
 * is comms_rx_handle_frame(), which performs structure + CRC + HMAC tag +
 * opcode + range
 * validation and dispatches only valid telecommands. A rejection reason must be
 * logged/telemetered, never discarded.
 */
void lora_rx_task(void *arg)
{
    (void)arg;

    /* Tell the driver which task to wake on RX_DONE. */
    lora_rx_task_register(osThreadGetId());

    /* Arm continuous RX so the DIO1 IRQ fires on the next downlink. */
    (void)lora_start_receive();

    for (;;) {
        /* Block on the DIO1 RX_DONE flag (with timeout) and kick the watchdog
           inside the loop so the blocking wait never arms a false 'hung' flag.
           The 100 ms poll granularity keeps the monitor happy. */
        uint32_t flags = osThreadFlagsWait(LORA_RX_FLAG, osFlagsWaitAny, 100U);
        watchdog_alive_self();

        if (flags == LORA_RX_FLAG) {
            size_t rx_len = 0U;
            uint8_t *rx = comms_rx_buffer(&rx_len);

            if (lora_rx(rx, &rx_len) == 0) {
                comms_tc_result_t r = comms_rx_handle_frame(rx, rx_len);
                if (r != COMMS_TC_OK) {
                    /* Rejection reason must be visible, not silently dropped.
                       Route it to the telemetry/log sink once one exists; for
                       now surface the human-readable reason via the existing
                       comms_tc_result_str(). */
                    const char *why = comms_tc_result_str(r);
                    (void)why;  /* TODO: forward `why` to telemetry/log sink */
                }
            } else {
                /* PHY-level reject (oversize payload or radio read error):
                   the frame never reached the validator, so account it here —
                   otherwise the drop is a blind spot in the RX stats. */
                comms_rx_account(COMMS_TC_ERR_PHY);
            }

            /* Re-arm RX for the next frame. */
            (void)lora_start_receive();
        }
    }
}

osThreadId_t lora_rx_task_create(void)
{
    osThreadId_t handle = osThreadNew(lora_rx_task, NULL, &rx_attrs);
    if (handle != NULL) {
        (void)watchdog_register_task(handle, WDG_PERIOD_LORA_RX_MS);
    }
    return handle;
}
