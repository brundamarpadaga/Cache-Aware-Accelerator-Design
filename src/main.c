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

    /* ── Check 1: SW matmul correctness at N=16 ─────────────────────────
     * A = identity, B = known sequential pattern.
     * Expected: C == B (identity × B = B).
     * N=16 matches the HW minimum valid size (multiple of FETCH_TILE=16). */
    {
#define SW_CHECK_N 16
        volatile float *A = (volatile float *)MAT_A_BASE;
        volatile float *B = (volatile float *)MAT_B_BASE;
        volatile float *C = (volatile float *)MAT_C_BASE;

        xil_printf("\r\nSW matmul correctness check (N=%d)...\r\n", SW_CHECK_N);
        for (int i = 0; i < SW_CHECK_N; i++)
            for (int j = 0; j < SW_CHECK_N; j++)
                A[i * SW_CHECK_N + j] = (i == j) ? 1.0f : 0.0f;
        for (int i = 0; i < SW_CHECK_N; i++)
            for (int j = 0; j < SW_CHECK_N; j++)
                B[i * SW_CHECK_N + j] = (float)(i * SW_CHECK_N + j + 1);
        for (int i = 0; i < SW_CHECK_N * SW_CHECK_N; i++) C[i] = 0.0f;

        for (int i = 0; i < SW_CHECK_N; i++)
            for (int k = 0; k < SW_CHECK_N; k++) {
                float a_ik = A[i * SW_CHECK_N + k];
                for (int j = 0; j < SW_CHECK_N; j++)
                    C[i * SW_CHECK_N + j] += a_ik * B[k * SW_CHECK_N + j];
            }

        int sw_errors = 0;
        for (int i = 0; i < SW_CHECK_N * SW_CHECK_N; i++) {
            if (C[i] != B[i]) {
                if (sw_errors < 16)
                    xil_printf("  SW MISMATCH [%d]: got 0x%08lX expected 0x%08lX\r\n",
                               i,
                               (unsigned long)*(uint32_t *)&C[i],
                               (unsigned long)*(uint32_t *)&B[i]);
                sw_errors++;
            }
        }
        if (sw_errors == 0)
            xil_printf("SW MATMUL PASS\r\n");
        else {
            xil_printf("SW MATMUL FAIL (%d errors)\r\n", sw_errors);
            while (1) {}
        }
    }

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

    /* ── Check 2: HW matmul correctness at all sweep sizes ──────────────
     * Same identity × known pattern as Check 1 at each N.
     * N must be a multiple of FETCH_TILE=16 and T_TILE=16.
     * Larger sizes (256, 512) add ~0.5 s and ~4 s respectively.          */
    {
        static const int hw_check_sizes[] = { 16, 32, 64, 128, 256, 512 };
        int num_hw_sizes = (int)(sizeof(hw_check_sizes) / sizeof(hw_check_sizes[0]));

        for (int si = 0; si < num_hw_sizes; si++) {
            int N = hw_check_sizes[si];
            int N2 = N * N;
            xil_printf("HW matmul correctness check (N=%d)...\r\n", N);

            for (int i = 0; i < N; i++)
                for (int j = 0; j < N; j++)
                    MAT_A[i * N + j] = (i == j) ? 1.0f : 0.0f;
            for (int i = 0; i < N; i++)
                for (int j = 0; j < N; j++)
                    MAT_B[i * N + j] = (float)(i * N + j + 1);

            Xil_DCacheFlushRange((UINTPTR)MAT_A_BASE, N2 * sizeof(float));
            Xil_DCacheFlushRange((UINTPTR)MAT_B_BASE, N2 * sizeof(float));
            for (int i = 0; i < N2; i++) MAT_C[i] = 0.0f;
            Xil_DCacheFlushRange((UINTPTR)MAT_C_BASE, N2 * sizeof(float));
            Xil_DCacheInvalidateRange((UINTPTR)MAT_B_T_BASE, N2 * sizeof(float));

            XMatmul_Set_A(&matmul_hw,   MAT_A_BASE);
            XMatmul_Set_B(&matmul_hw,   MAT_B_BASE);
            XMatmul_Set_B_T(&matmul_hw, MAT_B_T_BASE);
            XMatmul_Set_C(&matmul_hw,   MAT_C_BASE);
            XMatmul_Set_N(&matmul_hw,   N);
            XMatmul_Start(&matmul_hw);
            while (!XMatmul_IsDone(&matmul_hw));
            Xil_DCacheInvalidateRange((UINTPTR)MAT_C_BASE, N2 * sizeof(float));

            int hw_errors = 0;
            for (int i = 0; i < N2; i++) {
                if (MAT_C[i] != MAT_B[i]) {
                    if (hw_errors < 16)
                        xil_printf("  HW MISMATCH [%d]: got 0x%08lX expected 0x%08lX\r\n",
                                   i,
                                   (unsigned long)*(uint32_t *)&MAT_C[i],
                                   (unsigned long)*(uint32_t *)&MAT_B[i]);
                    hw_errors++;
                }
            }
            if (hw_errors == 0)
                xil_printf("HW MATMUL PASS (N=%d)\r\n", N);
            else {
                xil_printf("HW MATMUL FAIL (N=%d, %d errors)\r\n", N, hw_errors);
                while (1) {}
            }
        }
    }

    /* ── Check 3: SW vs HW cross-check with asymmetric inputs (N=32) ────
     * Both A and B are non-trivial, non-symmetric matrices.
     * SW computes the reference; HW must produce identical bit-exact output.
     * Catches row/column swap bugs that identity-based tests cannot detect.
     * N=32 is the smallest valid multi-tile size (> FETCH_TILE=16).       */
    {
#define CROSS_N 32
        volatile float *A = (volatile float *)MAT_A_BASE;
        volatile float *B = (volatile float *)MAT_B_BASE;
        volatile float *C = (volatile float *)MAT_C_BASE;
        /* 32×32 floats = 4 KB — safe on the Cortex-A9 stack. */
        float ref[CROSS_N * CROSS_N];

        xil_printf("SW vs HW cross-check with asymmetric inputs (N=%d)...\r\n", CROSS_N);

        /* Fill A and B with independent non-symmetric patterns.
         * A: row-major ascending;  B: column-biased (N-1-j reversal).
         * These patterns expose row/column swap bugs that identity tests miss. */
        for (int i = 0; i < CROSS_N; i++)
            for (int j = 0; j < CROSS_N; j++) {
                A[i * CROSS_N + j] = (float)(i * CROSS_N + j + 1);
                B[i * CROSS_N + j] = (float)((CROSS_N - 1 - j) * CROSS_N + i + 1);
            }
        for (int i = 0; i < CROSS_N * CROSS_N; i++) C[i] = 0.0f;

        /* SW reference: C = A × B (i-k-j order), saved into ref[] */
        for (int i = 0; i < CROSS_N; i++)
            for (int k = 0; k < CROSS_N; k++) {
                float a_ik = A[i * CROSS_N + k];
                for (int j = 0; j < CROSS_N; j++)
                    C[i * CROSS_N + j] += a_ik * B[k * CROSS_N + j];
            }
        for (int i = 0; i < CROSS_N * CROSS_N; i++) ref[i] = C[i];

        /* HW run on the same A, B — C is overwritten with HW result */
        Xil_DCacheFlushRange((UINTPTR)MAT_A_BASE, CROSS_N * CROSS_N * sizeof(float));
        Xil_DCacheFlushRange((UINTPTR)MAT_B_BASE, CROSS_N * CROSS_N * sizeof(float));
        for (int i = 0; i < CROSS_N * CROSS_N; i++) MAT_C[i] = 0.0f;
        Xil_DCacheFlushRange((UINTPTR)MAT_C_BASE, CROSS_N * CROSS_N * sizeof(float));
        Xil_DCacheInvalidateRange((UINTPTR)MAT_B_T_BASE, CROSS_N * CROSS_N * sizeof(float));

        XMatmul_Set_A(&matmul_hw,   MAT_A_BASE);
        XMatmul_Set_B(&matmul_hw,   MAT_B_BASE);
        XMatmul_Set_B_T(&matmul_hw, MAT_B_T_BASE);
        XMatmul_Set_C(&matmul_hw,   MAT_C_BASE);
        XMatmul_Set_N(&matmul_hw,   CROSS_N);
        XMatmul_Start(&matmul_hw);
        while (!XMatmul_IsDone(&matmul_hw));
        Xil_DCacheInvalidateRange((UINTPTR)MAT_C_BASE, CROSS_N * CROSS_N * sizeof(float));

        /* Allow up to 8 ULP difference — HW and SW accumulate in different
         * orders (tiled vs i-k-j) so rounding diverges by a few ULP.
         * A difference larger than 8 ULP indicates a real indexing bug. */
        int cross_errors = 0;
        for (int i = 0; i < CROSS_N * CROSS_N; i++) {
            uint32_t hw_bits = *(uint32_t *)&MAT_C[i];
            uint32_t sw_bits = *(uint32_t *)&ref[i];
            uint32_t ulp_diff = (hw_bits > sw_bits) ? (hw_bits - sw_bits)
                                                     : (sw_bits - hw_bits);
            if (ulp_diff > 8U) {
                if (cross_errors < 16)
                    xil_printf("  CROSS MISMATCH [%d]: HW=0x%08lX SW=0x%08lX (diff=%lu ULP)\r\n",
                               i,
                               (unsigned long)hw_bits,
                               (unsigned long)sw_bits,
                               (unsigned long)ulp_diff);
                cross_errors++;
            }
        }
        if (cross_errors == 0)
            xil_printf("SW vs HW CROSS-CHECK PASS\r\n");
        else {
            xil_printf("SW vs HW CROSS-CHECK FAIL (%d errors)\r\n", cross_errors);
            while (1) {}
        }
    }
#endif /* XPAR_MATMUL_0_DEVICE_ID */

    /* ── Benchmark sweep — do not call before smoke tests pass ───────── */
    xil_printf("\r\nStarting benchmark sweep...\r\n");
    run_benchmark_sweep();

    while (1) { /* halt */ }
    return 0;
}
