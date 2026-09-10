#ifndef SPI_DMA_SCHED_H
#define SPI_DMA_SCHED_H

/* spi_dma_sched.h — pure scheduling math for the SPI1 DMA transfer path
 * (radiolib_hal.cpp, SPF V3 §3.6.4.2). Header-only, no HAL / RadioLib /
 * FreeRTOS dependency, so the host Ceedling build can test it directly.
 *
 * Two facts drive the numbers:
 *  - one DMA transfer serves at most 0xFFFF bytes (16-bit NDTR);
 *  - the transfer time follows the SPI baud actually programmed
 *    (PCLK2 / BaudRatePrescaler divisor), not an assumed 10 Mbit/s.
 */

#include <stddef.h>
#include <stdint.h>

#define SPI_DMA_MAX_CHUNK 0xFFFFU

/* Number of DMA chunks for a transfer of len bytes (len == 0 -> 0). */
static inline uint32_t spi_dma_chunk_count(size_t len, uint32_t chunk_max)
{
    if ((len == 0U) || (chunk_max == 0U)) {
        return 0U;
    }
    return (uint32_t)(((uint64_t)len + (uint64_t)chunk_max - 1U) /
                      (uint64_t)chunk_max);
}

/* Timeout for ONE chunk of `chunk_len` bytes, in ms:
 *   wire time = chunk_len * 8 bit * presc_div / pclk_hz
 *   timeout   = wire time + margin_ms, floored at margin_ms.
 * 64-bit math: 65535 B * 8 * 256 / 1 Hz-class clocks cannot overflow. */
static inline uint32_t spi_dma_chunk_timeout_ms(uint32_t chunk_len,
                                                uint32_t pclk_hz,
                                                uint32_t presc_div,
                                                uint32_t margin_ms)
{
    uint64_t wire_ms;

    if ((pclk_hz == 0U) || (presc_div == 0U)) {
        return margin_ms;
    }
    wire_ms = ((uint64_t)chunk_len * 8U * (uint64_t)presc_div * 1000U) /
              (uint64_t)pclk_hz;
    return (uint32_t)(wire_ms + (uint64_t)margin_ms);
}

#endif /* SPI_DMA_SCHED_H */
