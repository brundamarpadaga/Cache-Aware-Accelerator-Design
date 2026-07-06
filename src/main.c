/**
 * @file   main.c
 * @author Brunda Marpadaga (brundamarpadaga@gmail.com)
 * @author Bhavana Marpadaga (marapadagabhavana@gmail.com)
 * @brief  ACP vs HP PS↔PL Coherency Benchmark — entry point.
 *
 * @details
 * Build target: Vitis bare-metal (Cortex-A9, Zynq-7000)
 * Toolchain:    arm-none-eabi-gcc
 *
 * Source files in this application project:
 *   main.c            — entry point (this file)
 *   pmu.h             — ARM PMU CP15 + PL310 L2 MMIO counter helpers
 *   dma_smoke_test.c  — XAxiDma instance, dma_init, dma_wait_done, smoke tests
 *   dma_smoke_test.h  — shared addresses, extern dma, extern dma_wait_done
 *   benchmark.c       — benchmark_coherent, benchmark_noncoherent, benchmark_matmul, sweep loop
 *   benchmark.h       — run_benchmark_sweep declaration
 *
 * @note Portions of this code were generated with assistance from Claude AI (Anthropic).
 *
 * @board  Digilent Zybo Z7-20 (XC7Z020-1CLG400C)
 * @tool   Vitis 2022.2, arm-none-eabi-gcc
 */

#include <stdint.h>
#include "xil_printf.h"
#include "xil_cache.h"
#include "pmu.h"
#include "dma_smoke_test.h"
#include "benchmark.h"
#ifdef XPAR_MATMUL_0_DEVICE_ID
#include "xmatmul.h"

/* Shared with benchmark.c — defined once here. */
XMatmul matmul_hw;

/* MAT_A/B/C base addresses — must match benchmark.c memory map. */
#define MAT_A_BASE    0x10000000UL
#define MAT_B_BASE    0x10400000UL
#define MAT_C_BASE    0x10800000UL
#define MAT_B_T_BASE  0x10C00000UL           /* B transposed scratch — HW writes here */
#define MAT_A  ((volatile float *)MAT_A_BASE)
#define MAT_B  ((volatile float *)MAT_B_BASE)
#define MAT_C  ((volatile float *)MAT_C_BASE)
#endif

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

#ifdef XPAR_MATMUL_0_DEVICE_ID
    /* ── HLS accelerator init ────────────────────────────────────────── */
    xil_printf("\r\nInitialising matmul_0 accelerator...\r\n");
    XMatmul_Config *matmul_cfg = XMatmul_LookupConfig(XPAR_MATMUL_0_DEVICE_ID);
    if (!matmul_cfg) {
        xil_printf("ERROR: XMatmul_LookupConfig returned NULL — "
                   "rebuild platform from new .xsa\r\n");
        while (1) { /* halt */ }
    }
    XMatmul_CfgInitialize(&matmul_hw, matmul_cfg);
    xil_printf("matmul_0 base addr : 0x%08lX\r\n",
               (unsigned long)XPAR_MATMUL_0_S_AXI_CONTROL_BASEADDR);

    /* ── Correctness check: 4×4 identity × known pattern (result must = B) */
    xil_printf("Running matmul correctness check (N=4)...\r\n");
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            MAT_A[i * 4 + j] = (i == j) ? 1.0f : 0.0f;   /* identity */
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            MAT_B[i * 4 + j] = (float)(i * 4 + j + 1);   /* known pattern */

    Xil_DCacheFlushRange((UINTPTR)MAT_A_BASE, 4 * 4 * sizeof(float));
    Xil_DCacheFlushRange((UINTPTR)MAT_B_BASE, 4 * 4 * sizeof(float));
    for (int i = 0; i < 4 * 4; i++) MAT_C[i] = 0.0f;
    Xil_DCacheFlushRange((UINTPTR)MAT_C_BASE, 4 * 4 * sizeof(float));
    /* Invalidate B_T scratch region — HW writes it via HP0 then reads via ACP.
     * Without this, SCU may serve stale PS cache lines at 0x10C00000 instead
     * of the freshly written DDR data, producing NaN results.               */
    Xil_DCacheInvalidateRange((UINTPTR)MAT_B_T_BASE, 4 * 4 * sizeof(float));

    XMatmul_Set_A(&matmul_hw,   MAT_A_BASE);
    XMatmul_Set_B(&matmul_hw,   MAT_B_BASE);
    XMatmul_Set_B_T(&matmul_hw, MAT_B_T_BASE);
    XMatmul_Set_C(&matmul_hw,   MAT_C_BASE);
    XMatmul_Set_N(&matmul_hw,   4);
    XMatmul_Start(&matmul_hw);
    while (!XMatmul_IsDone(&matmul_hw));

    Xil_DCacheInvalidateRange((UINTPTR)MAT_C_BASE, 4 * 4 * sizeof(float));

    int errors = 0;
    for (int i = 0; i < 4 * 4; i++) {
        if (MAT_C[i] != MAT_B[i]) {
            xil_printf("  MISMATCH [%d]: got 0x%08lX expected 0x%08lX\r\n",
                       i,
                       (unsigned long)*(uint32_t *)&MAT_C[i],
                       (unsigned long)*(uint32_t *)&MAT_B[i]);
            errors++;
        }
    }
    if (errors == 0) {
        xil_printf("MATMUL PASS\r\n");
    } else {
        xil_printf("MATMUL FAIL (%d errors) - do not trust benchmark results\r\n",
                   errors);
        while (1) { /* halt */ }
    }
#endif /* XPAR_MATMUL_0_DEVICE_ID */

    /* ── Benchmark sweep — do not call before smoke tests pass ───────── */
    xil_printf("\r\nStarting benchmark sweep...\r\n");
    run_benchmark_sweep();

    while (1) { /* halt */ }
    return 0;
}
