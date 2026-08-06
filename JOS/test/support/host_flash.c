/* Host-side Flash region simulator — see support/host_flash.h. */
#define _GNU_SOURCE
#include "host_flash.h"

#include <sys/mman.h>
#include <string.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000   /* Linux >= 4.17 */
#endif

void *host_flash_map(uint32_t base, size_t size)
{
    void *p = mmap((void *)(uintptr_t)base, size,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);

    if (p == MAP_FAILED) {
        return NULL;
    }
    if (p != (void *)(uintptr_t)base) {
        /* The kernel ignored the hint: the region is unusable for this test. */
        (void)munmap(p, size);
        return NULL;
    }
    memset(p, 0xFF, size);   /* erased Flash reads as all-ones */
    return p;
}

void host_flash_unmap(void *addr, size_t size)
{
    if (addr != NULL) {
        (void)munmap(addr, size);
    }
}
