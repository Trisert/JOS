/* ---------------------------------------------------------------------------
 * test_spi_dma.c — scheduling math of the SPI1 DMA transfer path
 * (App/comms/spi_dma_sched.h, SPF V3 §3.6.4.2).
 *
 * Pure integer helpers, no HAL: chunk counts and per-chunk timeouts derived
 * from the programmed SPI baud instead of an assumed bit rate.
 * ------------------------------------------------------------------------- */
#include "unity.h"
#include "spi_dma_sched.h"

/* --- chunk counts ------------------------------------------------------- */

void test_dma_chunk_count_zero_len_needs_no_chunk(void)
{
    TEST_ASSERT_EQUAL_UINT32(0U, spi_dma_chunk_count(0U, SPI_DMA_MAX_CHUNK));
}

void test_dma_chunk_count_single_byte_needs_one_chunk(void)
{
    TEST_ASSERT_EQUAL_UINT32(1U, spi_dma_chunk_count(1U, SPI_DMA_MAX_CHUNK));
}

void test_dma_chunk_count_exact_max_is_one_chunk(void)
{
    TEST_ASSERT_EQUAL_UINT32(1U, spi_dma_chunk_count(SPI_DMA_MAX_CHUNK,
                                                     SPI_DMA_MAX_CHUNK));
}

void test_dma_chunk_count_max_plus_one_is_two_chunks(void)
{
    TEST_ASSERT_EQUAL_UINT32(2U, spi_dma_chunk_count(SPI_DMA_MAX_CHUNK + 1U,
                                                     SPI_DMA_MAX_CHUNK));
}

void test_dma_chunk_count_64kib_needs_two_chunks(void)
{
    /* 65536 B (the old 64 KiB chunking) no longer fits one NDTR window. */
    TEST_ASSERT_EQUAL_UINT32(2U, spi_dma_chunk_count(65536U, SPI_DMA_MAX_CHUNK));
}

void test_dma_chunk_count_zero_max_is_safe(void)
{
    TEST_ASSERT_EQUAL_UINT32(0U, spi_dma_chunk_count(100U, 0U));
}

/* --- per-chunk timeouts ---------------------------------------------------
 * Reference: SPI1 = PCLK2 80 MHz / prescaler 8 = 10 Mbit/s (MX_SPI1_Init).
 * 65535 B on the wire = 65535*8/10e6 s = 52.4 ms. */

void test_dma_timeout_full_chunk_at_10mhz(void)
{
    uint32_t t = spi_dma_chunk_timeout_ms(65535U, 80000000U, 8U, 25U);
    TEST_ASSERT_UINT32_WITHIN(2U, 77U, t);   /* 52.4 ms wire + 25 ms margin */
}

void test_dma_timeout_small_frame_is_mostly_margin(void)
{
    uint32_t t = spi_dma_chunk_timeout_ms(16U, 80000000U, 8U, 25U);
    TEST_ASSERT_EQUAL_UINT32(25U, t);        /* 12.8 us wire, margin floors */
}

void test_dma_timeout_scales_with_slower_baud(void)
{
    uint32_t fast = spi_dma_chunk_timeout_ms(65535U, 80000000U, 8U, 25U);
    uint32_t slow = spi_dma_chunk_timeout_ms(65535U, 80000000U, 256U, 25U);
    /* 32x slower clock -> ~32x wire time on top of the same margin. */
    TEST_ASSERT_UINT32_WITHIN(60U, (slow - fast), 32U * 52U);
    TEST_ASSERT_TRUE(slow > fast);
}

void test_dma_timeout_zero_clock_falls_back_to_margin(void)
{
    TEST_ASSERT_EQUAL_UINT32(25U, spi_dma_chunk_timeout_ms(100U, 0U, 8U, 25U));
    TEST_ASSERT_EQUAL_UINT32(25U, spi_dma_chunk_timeout_ms(100U, 80000000U, 0U, 25U));
}
