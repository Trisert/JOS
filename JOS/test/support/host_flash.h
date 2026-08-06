/**
 * @file    support/host_flash.h
 * @brief   Host-side simulation of the STM32L4 internal Flash region used by
 *          the LastStates pool.
 *
 * App/memory/memory.c programs and reads the pool through absolute addresses
 * (0x08080000..0x08082000). On the test host those addresses are simply
 * unmapped, so the helper below maps anonymous memory *at that exact address*
 * and the HAL_FLASH_Program mock writes into it. That keeps the production
 * code path unmodified while still allowing a real write -> read round trip.
 *
 * Isolated in support/ because Ceedling's test-runner generator rewrites
 * `#include <sys/...>` directives found in a test file and cannot resolve them.
 */
#ifndef JOS_TEST_HOST_FLASH_H
#define JOS_TEST_HOST_FLASH_H

#include <stdint.h>
#include <stddef.h>

/** Map @p size bytes of writable memory at absolute address @p base.
 *  @return the mapped pointer, or NULL if the address could not be reserved. */
void *host_flash_map(uint32_t base, size_t size);

/** Release a region previously returned by host_flash_map(). */
void host_flash_unmap(void *addr, size_t size);

#endif /* JOS_TEST_HOST_FLASH_H */
