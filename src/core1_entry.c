/**
 * @file   core1_entry.c
 * @author Brunda Marpadaga (brundamarpadaga@gmail.com)
 * @brief  Core 1 bare-metal SW matmul sweep with PMU/L2 probing.
 *
 * @details
 * This file is the standalone application for ps7_cortexa9_1.
 * It is built as a separate Vitis application project (standalone BSP,
 * CPU1 domain) and loaded by the system project debugger alongside the
 * FreeRTOS CPU0 application.
 *
 * Startup sequence:
 *   - CPU1's own _start (from standalone BSP) sets up stack and calls main()
 *   - main() polls CORE1_CMD until Core 0 writes CORE1_CMD_RUN
 *   - Core 0 flushes the comm region to DDR before sending SEV
 *   - Core 1 invalidates its cache before reading the flag
 *
 * Each matmul rep is bracketed:
 *   pmu_reset_counters() + l2_reset()
 *   XTime_GetTime(&t0) -> matmul loop -> XTime_GetTime(&t1)
 *   pmu_read_all() + l2_read()
 *
 * L2 values are shared with Core 0 and may include its idle activity.
 * L1 values are Core 1's own CP15 counters and are accurate.
 *
 * Linker script: place code at 0x20000000 (non-overlapping with CPU0).
 *
 * @note Portions of this file were developed with assistance from
 *       Claude AI (Anthropic) - two-project AMP structure, cache-invalidating
 *       poll for inter-core synchronisation, PMU/L2 instrumentation.
 *
 * @board  Digilent Zybo Z7-20 (XC7Z020-1CLG400C)
 * @tool   Vitis 2022.2, arm-none-eabi-gcc
 */

#include <stdint.h>
#include "xil_printf.h"
#include "xil_cache.h"
#include "xtime_l.h"
#include "pmu.h"

/* Shared communication region (must match freertos_main.c) */
#define CORE1_COMM_BASE     0x10F00000UL
#define CORE1_CMD           (*(volatile uint32_t *)(CORE1_COMM_BASE + 0x00))
#define CORE1_STATUS        (*(volatile uint32_t *)(CORE1_COMM_BASE + 0x04))
#define CORE1_ELAPSED_US    (*(volatile uint64_t *)(CORE1_COMM_BASE + 0x08))

#define CORE1_CMD_IDLE      0x00000000UL
#define CORE1_CMD_RUN       0xDEAD0001UL
#define CORE1_STATUS_BUSY   0x00000000UL
#define CORE1_STATUS_DONE   0xCAFE0001UL

/* Separate DDR region from Core 0's matrices to avoid contention.
 * Core 0 active/staging buffers are at 0x10xxxxxx and 0x14xxxxxx. */
#define SW_A_BASE   0x11000000UL
#define SW_B_BASE   0x11400000UL
#define SW_C_BASE   0x11800000UL

#define SW_A  ((volatile float *)SW_A_BASE)
#define SW_B  ((volatile float *)SW_B_BASE)
#define SW_C  ((volatile float *)SW_C_BASE)

static const uint32_t SIZES[] = { 32U, 64U, 128U, 256U, 512U };
#define NUM_SIZES  (sizeof(SIZES) / sizeof(SIZES[0]))
#define REPEATS    5U

static inline uint64_t ticks_to_us(XTime ticks)
{
    return (uint64_t)ticks * 1000000ULL / (uint64_t)COUNTS_PER_SECOND;
}

int main(void)
{
    /* Poll until Core 0 writes CORE1_CMD_RUN and flushes to DDR.
     * Invalidate before each read - both cores have caches enabled in the
     * two-project setup, so a plain volatile read may hit Core 1's stale L1. */
    do {
        Xil_DCacheInvalidateRange((UINTPTR)CORE1_COMM_BASE, 16UL);
    } while (CORE1_CMD != CORE1_CMD_RUN);

    pmu_init();
    l2_init();

    xil_printf("[Core1] Started SW matmul sweep\r\n");

    XTime sweep_start, sweep_end;
    XTime_GetTime(&sweep_start);

    for (uint32_t si = 0; si < NUM_SIZES; si++) {
        uint32_t N     = SIZES[si];
        uint32_t N2    = N * N;
        uint32_t bytes = N2 * sizeof(float);

        for (uint32_t i = 0; i < N2; i++) SW_A[i] = 1.0f + (float)i * 0.001f;
        for (uint32_t i = 0; i < N2; i++) SW_B[i] = 2.0f + (float)i * 0.001f;
        Xil_DCacheFlushRange((UINTPTR)SW_A_BASE, bytes);
        Xil_DCacheFlushRange((UINTPTR)SW_B_BASE, bytes);

        for (uint32_t rep = 0; rep < REPEATS; rep++) {
            for (uint32_t i = 0; i < N2; i++) SW_C[i] = 0.0f;
            Xil_DCacheFlushRange((UINTPTR)SW_C_BASE, bytes);

            /* Reset Core 1 L1 PMU and shared L2 immediately before the loop.
             * L2 values are approximate - Core 0 idle tasks may contribute. */
            pmu_reset_counters();
            l2_reset();

            XTime t0, t1;
            XTime_GetTime(&t0);

            /* C = A x B  (i-k-j order, cache-friendly) */
            for (uint32_t i = 0; i < N; i++)
                for (uint32_t k = 0; k < N; k++) {
                    float a_ik = SW_A[i * N + k];
                    for (uint32_t j = 0; j < N; j++)
                        SW_C[i * N + j] += a_ik * SW_B[k * N + j];
                }

            __asm__ volatile("dsb" ::: "memory");
            XTime_GetTime(&t1);

            pmu_counts_t pmu;
            l2_counts_t  l2;
            pmu_read_all(&pmu);
            l2_read(&l2);

            uint64_t us = ticks_to_us(t1 - t0);

            uint32_t l1_hit_pct = (pmu.l1d_access > 0U)
                ? (uint32_t)(((pmu.l1d_access - pmu.l1d_miss) * 100ULL)
                              / pmu.l1d_access) : 0U;
            uint32_t l2_rate_pct = (l2.drreq > 0U)
                ? (uint32_t)((l2.drhit * 100ULL) / l2.drreq) : 0U;

            xil_printf("SW_CORE1,%lu,%lu,"
                       "L1acc=%lu,L1miss=%lu,L1pct=%lu,"
                       "L2req=%lu,L2hit=%lu,L2pct_approx=%lu\r\n",
                       (unsigned long)N,
                       (unsigned long)us,
                       (unsigned long)pmu.l1d_access,
                       (unsigned long)pmu.l1d_miss,
                       (unsigned long)l1_hit_pct,
                       (unsigned long)l2.drreq,
                       (unsigned long)l2.drhit,
                       (unsigned long)l2_rate_pct);
        }
    }

    XTime_GetTime(&sweep_end);
    CORE1_ELAPSED_US = ticks_to_us(sweep_end - sweep_start);
    __asm__ volatile("dsb" ::: "memory");

    /* Flush so Core 0 sees the elapsed time and done flag from DDR */
    Xil_DCacheFlushRange((UINTPTR)CORE1_COMM_BASE, 16UL);
    CORE1_STATUS = CORE1_STATUS_DONE;
    Xil_DCacheFlushRange((UINTPTR)CORE1_COMM_BASE, 16UL);

    xil_printf("[Core1] SW sweep done. Total: %lu us\r\n",
               (unsigned long)CORE1_ELAPSED_US);

    while (1)
        __asm__ volatile("wfe");

    return 0;
}
