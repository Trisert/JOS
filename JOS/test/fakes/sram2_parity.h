/**
 * @file    fakes/sram2_parity.h
 * @brief   Host-test stand-in for Core/Inc/sram2_parity.h (W2-3).
 *
 * App/comms/comms.c only uses the *placement* macros from this header: its
 * packet staging buffers are declared SRAM2_CRITICAL_NOINIT so that a bit flip
 * in a frame being assembled or decoded raises a parity NMI on target.
 *
 * On the host there is no SRAM2 and no parity hardware, so the attributes
 * expand to nothing and the buffers become ordinary statics. Emitting the real
 * `__attribute__((section(".sram2_noinit")))` here would put objects into a
 * section the host linker script knows nothing about and would silently break
 * gcov instrumentation of the translation unit.
 *
 * The functional API (sram2_parity_init(), the NMI handlers, the .noinit
 * store) is deliberately NOT declared: no host-compiled module calls it, and a
 * declaration with no definition on the :support: path is a link error waiting
 * for the next test that includes this header.
 */
#ifndef JOS_TEST_FAKE_SRAM2_PARITY_H
#define JOS_TEST_FAKE_SRAM2_PARITY_H

#define SRAM2_CRITICAL
#define SRAM2_CRITICAL_NOINIT

#endif /* JOS_TEST_FAKE_SRAM2_PARITY_H */
