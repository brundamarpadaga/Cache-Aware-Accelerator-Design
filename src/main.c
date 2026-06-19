/**
 * main.c — ACP vs HP PS↔PL Coherency Benchmark
 * Member B: PS Software Engineer
 *
 * Build target: Vitis bare-metal (Cortex-A9, Zynq-7000)
 * Toolchain:    arm-none-eabi-gcc
 *
 * Source files in this application project:
 *   main.c            — entry point (this file)
 *   pmu.h             — ARM PMU CP15 + PL310 L2 MMIO counter helpers
 *   dma_smoke_test.c  — XAxiDma instance, dma_init, dma_wait_done, smoke tests
 *   dma_smoke_test.h  — shared addresses, extern dma, extern dma_wait_done
 *   benchmark.c       — benchmark_coherent, benchmark_noncoherent, sweep loop
 *   benchmark.h       — run_benchmark_sweep declaration
 */

#include <stdint.h>
#include "xil_printf.h"
#include "pmu.h"
#include "dma_smoke_test.h"
#include "benchmark.h"

int main(void)
{
    xil_printf("Hello from Zybo Z7-20\r\n");

    /* ── Counter init ─────────────────────────────────────────────────── */
    pmu_init();
    l2_init();
    xil_printf("PMU and PL310 L2 counters initialised\r\n");

    uint32_t l2_id   = *(volatile uint32_t *)(0xF8F02000UL + 0x000U);
    uint32_t l2_ctrl = *(volatile uint32_t *)(0xF8F02000UL + 0x100U);
    xil_printf("PL310 Cache ID   : 0x%08lX\r\n", (unsigned long)l2_id);
    xil_printf("PL310 Ctrl (en?) : 0x%08lX\r\n", (unsigned long)l2_ctrl);

    /* ── Counter sanity check ─────────────────────────────────────────── */
    {
        /* Use MAT_A region (0x10000000) — same as benchmark, safe address. */
        volatile float *buf = (volatile float *)0x10000000UL;
        pmu_counts_t before, after;
        l2_counts_t  l2b, l2a;

        l2_reset();
        pmu_read_all(&before);
        l2_read(&l2b);

        for (int i = 0; i < 16384; i++) buf[i] = (float)i;
        for (int i = 0; i < 16384; i++) buf[i] *= 2.0f;

        pmu_read_all(&after);
        l2_read(&l2a);

        xil_printf("L1 access delta   : %lu\r\n",
                   (unsigned long)(after.l1d_access - before.l1d_access));
        xil_printf("L1 miss delta     : %lu\r\n",
                   (unsigned long)(after.l1d_miss   - before.l1d_miss));
        xil_printf("L2 access (DRREQ) : %lu\r\n",
                   (unsigned long)(l2a.drreq - l2b.drreq));
        xil_printf("L2 hit (DRHIT)    : %lu\r\n",
                   (unsigned long)(l2a.drhit - l2b.drhit));
    }

    /* ── Smoke tests — initialises `dma`, confirms HW wiring ─────────── */
    run_dma_smoke_tests();

    /* ── Benchmark sweep — do not call before smoke tests pass ───────── */
    xil_printf("\r\nStarting benchmark sweep...\r\n");
    run_benchmark_sweep();

    while (1) { /* halt */ }
    return 0;
}
