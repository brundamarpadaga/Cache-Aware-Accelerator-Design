/**
 * @file   core1_entry.c
 * @author Brunda Marpadaga (brundamarpadaga@gmail.com)
 * @brief  Core 1 bare-metal entry point - SW matmul sweep.
 *
 * @details
 * Core 1 boots and waits in WFE until Core 0 writes CORE1_CMD_RUN and
 * broadcasts an SEV. It then runs the full SW NxN matmul sweep
 * independently of Core 0 and FreeRTOS.
 *
 * Communication with Core 0 is via uncached DDR flags at CORE1_COMM_BASE.
 * No FreeRTOS - Core 1 runs bare-metal throughout.
 *
 * To register this as Core 1's entry point in Vitis:
 *   In the AMP BSP configuration, set the CPU1 start address to core1_main.
 *   Alternatively write the entry address to the CPU1 start register:
 *     *(uint32_t*)0xFFFFFFF0 = (uint32_t)core1_main;
 *
 * @note Portions of this file were developed with assistance from
 *       Claude AI (Anthropic) - Core 1 WFE boot sequence, shared DDR
 *       communication protocol, and SW matmul loop structure.
 *
 * @board  Digilent Zybo Z7-20 (XC7Z020-1CLG400C)
 * @tool   Vitis 2022.2, arm-none-eabi-gcc
 */

#include <stdint.h>
#include "xil_printf.h"
#include "xil_cache.h"
#include "xtime_l.h"

/* Shared communication region (must match freertos_main.c) */
#define CORE1_COMM_BASE     0x10F00000UL
#define CORE1_CMD           (*(volatile uint32_t *)(CORE1_COMM_BASE + 0x00))
#define CORE1_STATUS        (*(volatile uint32_t *)(CORE1_COMM_BASE + 0x04))
#define CORE1_ELAPSED_US    (*(volatile uint64_t *)(CORE1_COMM_BASE + 0x08))

#define CORE1_CMD_IDLE      0x00000000UL
#define CORE1_CMD_RUN       0xDEAD0001UL
#define CORE1_STATUS_BUSY   0x00000000UL
#define CORE1_STATUS_DONE   0xCAFE0001UL

/* Separate DDR region from Core 0's matrices to avoid contention */
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

void core1_main(void)
{
    /* Wait for Core 0 to signal start */
    while (CORE1_CMD != CORE1_CMD_RUN)
        __asm__ volatile("wfe");

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

            uint64_t us = ticks_to_us(t1 - t0);
            xil_printf("SW_CORE1,%lu,%lu\r\n",
                       (unsigned long)N, (unsigned long)us);
        }
    }

    XTime_GetTime(&sweep_end);
    CORE1_ELAPSED_US = ticks_to_us(sweep_end - sweep_start);

    __asm__ volatile("dsb" ::: "memory");
    CORE1_STATUS = CORE1_STATUS_DONE;

    xil_printf("[Core1] SW sweep done. Total: %lu us\r\n",
               (unsigned long)CORE1_ELAPSED_US);

    while (1)
        __asm__ volatile("wfe");
}
