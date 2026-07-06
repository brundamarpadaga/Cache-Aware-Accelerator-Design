/**
 * @file   benchmark.c
 * @author Brunda Marpadaga (brundamarpadaga@gmail.com)
 * @author Bhavana Marpadaga (marapadagabhavana@gmail.com)
 * @brief  ACP vs HP0 PS@htmlonly↔@endhtmlonly PL coherency benchmark — sweep functions.
 *
 * @details
 * Implements the two benchmark paths and the outer sweep loop.
 * Separated from main.c so the benchmark logic is self-contained and
 * main.c remains a thin entry-point.
 *
 * @par DMA instance sharing
 * The ::XAxiDma instance @c dma is defined (non-static) in dma_smoke_test.c
 * and initialised once by run_dma_smoke_tests() -> dma_init().
 * This file accesses it via the @c extern declaration in dma_smoke_test.h —
 * do @b not call XAxiDma_CfgInitialize() again from here.
 *
 * @par Reuse from dma_smoke_test.c
 * - @c dma           — shared ::XAxiDma instance (extern, non-static)
 * - dma_wait_done()  — polls MM2S SR (offset 0x04) then S2MM SR (offset 0x34)
 *                      bit[1]; issues a DSB before returning.
 *                      Returns 0 on success, -1 on timeout.
 *
 * @par Hardware port mapping (fixed in Vivado block design)
 * | AXI DMA channel | Protocol converter | PS port   | Coherency    |
 * |:----------------|:-------------------|:----------|:-------------|
 * | MM2S (read)     | AXI-PC             | S_AXI_ACP | SCU snooping |
 * | S2MM (write)    | AXI-PC             | S_AXI_HP0 | Non-coherent |
 *
 * AXI stream loopback: M_AXIS_MM2S -> S_AXIS_S2MM (accelerator placeholder).
 * ARUSER/ARCACHE = 0xF must be set on the ACP protocol converter.
 *
 * @par CSV output format
 * One row per run; lines beginning with @c # are comments for the parser.
 * @code
 * mode,N,elapsed_us,l1d_miss,l1d_access,l2d_miss,l2d_access,l1_hit_pct,l2_hit_pct
 * @endcode
 *
 * @note Feed captured UART log to:
 *       @code python3 plot_results.py results.log --out figures/ @endcode
 *
 *       Portions of this code, debugging assistance, and documentation
 *       were developed with help from Claude (Anthropic).
 *
 * @board  Digilent Zybo Z7-20 (XC7Z020-1CLG400C)
 * @tool   Vitis 2022.2, arm-none-eabi-gcc
 */

#include <stdint.h>
#include "xaxidma.h"
#include "xil_printf.h"
#include "xil_cache.h"
#include "xtime_l.h"
#include "xparameters.h"
#include "pmu.h"
#include "dma_smoke_test.h"   /* extern dma, dma_wait_done(), DDR addresses */
#include "benchmark.h"
#ifdef XPAR_MATMUL_0_DEVICE_ID
#include "xmatmul.h"
extern XMatmul matmul_hw;    /* defined and initialised in main.c */
#endif

/* ── Matmul mode selection ───────────────────────────────────────────────────
 * Choose which matmul benchmark runs in the sweep.  Set via Vitis project
 * build settings: C/C++ Build → Settings → Compiler → Preprocessor Symbols
 *
 *   MATMUL_MODE=MATMUL_SW   (default) — ARM software multiply
 *   MATMUL_MODE=MATMUL_HW            — Vitis HLS accelerator (requires new XSA)
 *
 * Only one mode runs per sweep to keep total runtime manageable.
 * ─────────────────────────────────────────────────────────────────────────── */
#define MATMUL_SW  1
#define MATMUL_HW  2
#ifndef MATMUL_MODE
#define MATMUL_MODE  MATMUL_SW
#endif

/* If HW mode is requested but the platform hasn't been rebuilt yet,
 * fall back to SW silently so the project still compiles. */
#if MATMUL_MODE == MATMUL_HW && !defined(XPAR_MATMUL_0_DEVICE_ID)
#warning "MATMUL_MODE=MATMUL_HW set but xmatmul.h not found — falling back to SW until XSA is updated"
#undef  MATMUL_MODE
#define MATMUL_MODE  MATMUL_SW
#endif

/* ── Memory barriers ─────────────────────────────────────────────────────── */
#define DSB()  __asm__ volatile("dsb" ::: "memory")
#define DMB()  __asm__ volatile("dmb" ::: "memory")

/* ─────────────────────────────────────────────────────────────────────────────
 * DDR MEMORY MAP  (must match dma_smoke_test.h and main.c)
 *
 * SRC_BASE     (0x10000000) = matrix A       — DMA source / HLS A input,  4 MB
 * MAT_B_BASE   (0x10400000) = matrix B       — HLS B input,               4 MB
 * DST_BASE     (0x10800000) = matrix C       — HLS result / DMA dest,     4 MB
 * MAT_B_T_BASE (0x10C00000) = B transposed   — HLS internal scratch (NEW), 4 MB
 *   The HLS kernel transposes B into this region internally before tiling,
 *   so B access becomes row-major (burst-friendly) instead of column-strided.
 *   No software transpose needed — do NOT write to this region from the PS.
 * ───────────────────────────────────────────────────────────────────────────*/
#define SRC_BASE      SMOKE_SRC_BASE         /* 0x10000000 — matrix A / DMA source */
#define MAT_B_BASE    0x10400000UL           /* 0x10400000 — matrix B              */
#define DST_BASE      SMOKE_DST_BASE         /* 0x10800000 — matrix C / DMA dest   */
#define MAT_B_T_BASE  0x10C00000UL           /* 0x10C00000 — B transposed scratch  */

#define SRC      ((volatile float *)SRC_BASE)
#define MAT_B    ((volatile float *)MAT_B_BASE)
#define DST      ((volatile float *)DST_BASE)
#define MAT_B_T  ((volatile float *)MAT_B_T_BASE)

/* ─────────────────────────────────────────────────────────────────────────────
 * BENCHMARK PARAMETERS
 * ───────────────────────────────────────────────────────────────────────────*/
static const uint32_t SIZES[] = { 32U, 64U, 128U, 256U, 512U };
#define NUM_SIZES  (sizeof(SIZES) / sizeof(SIZES[0]))

/* Back-to-back runs per (mode, N); Python post-processor takes the median. */
#define REPEATS  5U

/* Cache-line granularity for aligned flush/invalidate. */
#define CACHELINE  64U

/* ─────────────────────────────────────────────────────────────────────────────
 * INTERNAL HELPERS
 * ───────────────────────────────────────────────────────────────────────────*/

/** Round byte count up to next CACHELINE multiple for aligned cache ops. */
static inline uint32_t align_up(uint32_t n)
{
    return (n + CACHELINE - 1U) & ~(CACHELINE - 1U);
}

/** Convert Global Timer ticks to microseconds.
 *  Timer runs at CPU_CLK/2; COUNTS_PER_SECOND defined by xtime_l.h. */
static inline uint64_t ticks_to_us(XTime ticks)
{
    return (uint64_t)ticks * 1000000ULL / (uint64_t)COUNTS_PER_SECOND;
}

/** Fill an N×N region with a deterministic float pattern.
 *  Volatile pointer prevents the compiler discarding the stores. */
static void mat_fill(volatile float *m, uint32_t N, float seed)
{
    for (uint32_t i = 0U; i < N * N; ++i)
        m[i] = seed + (float)i * 0.001f;
}

/**
 * Emit one CSV row over UART.
 *
 * l2d_miss is derived as (drreq - drhit) because the PL310 exposes only
 * read-request and read-hit counters, not a direct miss counter.
 *
 * xil_printf has no %llu or %f support, so:
 *  - Hit rates are computed as integer × 10000, then split into
 *    integer and two-decimal-place fractional parts for printing.
 *  - All values are cast to unsigned long for %lu.
 */
static void print_csv_row(const char     *mode,
                           uint32_t        N,
                           uint64_t        elapsed_us,
                           const pmu_counts_t *ps,
                           const pmu_counts_t *pe,
                           const l2_counts_t  *ls,
                           const l2_counts_t  *le)
{
    uint32_t l1_miss   = pe->l1d_miss   - ps->l1d_miss;
    uint32_t l1_access = pe->l1d_access - ps->l1d_access;
    uint32_t l2_req    = le->drreq      - ls->drreq;
    uint32_t l2_hit    = le->drhit      - ls->drhit;
    uint32_t l2_miss   = (l2_req > l2_hit) ? (l2_req - l2_hit) : 0U;

    /* × 10000 gives two decimal places without floating-point printf */
    uint32_t l1_x100 = (l1_access > 0U)
        ? (uint32_t)(((uint64_t)(l1_access - l1_miss) * 10000ULL) / l1_access)
        : 0U;
    uint32_t l2_x100 = (l2_req > 0U)
        ? (uint32_t)(((uint64_t)l2_hit * 10000ULL) / l2_req)
        : 0U;

    xil_printf("%s,%lu,%lu,%lu,%lu,%lu,%lu,%lu.%02lu,%lu.%02lu\r\n",
               mode,
               (unsigned long)N,
               (unsigned long)elapsed_us,
               (unsigned long)l1_miss,
               (unsigned long)l1_access,
               (unsigned long)l2_miss,
               (unsigned long)l2_req,
               (unsigned long)(l1_x100 / 100U),
               (unsigned long)(l1_x100 % 100U),
               (unsigned long)(l2_x100 / 100U),
               (unsigned long)(l2_x100 % 100U));
}

/* ─────────────────────────────────────────────────────────────────────────────
 * BENCHMARK PATHS
 * ───────────────────────────────────────────────────────────────────────────*/

/**
 * benchmark_coherent() — ACP read path, no SRC cache management.
 *
 * SRC (SRC) is left dirty in L1/L2 after mat_fill — it is NOT flushed.
 * The DMA reads SRC via ACP; the SCU snoops PS cache and serves the
 * dirty lines directly, so DDR is never consulted for source data.
 *
 * DST (DST) zeros are flushed before the transfer to prevent a
 * speculative PS cache writeback overwriting the HP0 result in DDR
 * after S2MM has already committed it.
 *
 * DST is invalidated after dma_wait_done() because S2MM always writes
 * via HP0 (hardwired), bypassing the SCU — the PS cache still holds the
 * pre-transfer zeros and must be discarded before reading the result.
 *
 * Timed interval: S2MM arm → MM2S start → both channels idle → invalidate.
 *
 * Returns elapsed microseconds, or UINT64_MAX on DMA error/timeout.
 */
static uint64_t benchmark_coherent(uint32_t N,
                                    pmu_counts_t *ps, pmu_counts_t *pe,
                                    l2_counts_t  *ls, l2_counts_t  *le)
{
    uint32_t bytes = align_up(N * N * (uint32_t)sizeof(float));
    XTime t0, t1;

    /* ── Untimed setup ────────────────────────────────────────────────────
     * Fill SRC dirty into cache — deliberately no flush.
     * Clear DST then flush its zeros to DDR so the HP0 write landing
     * in DDR cannot be overwritten by a later speculative writeback.     */
    mat_fill(SRC, N, 1.0f);

    for (uint32_t i = 0U; i < N * N; ++i) DST[i] = 0.0f;
    Xil_DCacheFlushRange((UINTPTR)DST_BASE, bytes);

    DMB();   /* all preceding stores globally visible before timed region */

    /* ── Timed region ─────────────────────────────────────────────────── */
    pmu_reset_counters();
    l2_reset();
    pmu_read_all(ps);
    l2_read(ls);
    XTime_GetTime(&t0);

    /* S2MM first — arm the receiver before MM2S pushes onto AXI stream.
     * If MM2S starts first the stream stalls and MM2S hangs indefinitely. */
    int rc = XAxiDma_SimpleTransfer(&dma,
                                    (UINTPTR)DST_BASE,
                                    bytes,
                                    XAXIDMA_DEVICE_TO_DMA);  /* S2MM */
    if (rc != XST_SUCCESS) {
        xil_printf("# ERROR: ACP S2MM failed (N=%lu rc=%d)\r\n",
                   (unsigned long)N, rc);
        return UINT64_MAX;
    }

    /* MM2S: DMA reads SRC via ACP — SCU snoops, serves dirty cache lines */
    rc = XAxiDma_SimpleTransfer(&dma,
                                (UINTPTR)SRC_BASE,
                                bytes,
                                XAXIDMA_DMA_TO_DEVICE);      /* MM2S */
    if (rc != XST_SUCCESS) {
        xil_printf("# ERROR: ACP MM2S failed (N=%lu rc=%d)\r\n",
                   (unsigned long)N, rc);
        return UINT64_MAX;
    }

    /* Reused from dma_smoke_test.c — polls SR bit[1] then issues DSB */
    if (dma_wait_done() != 0) {
        xil_printf("# WARNING: ACP timeout (N=%lu)\r\n", (unsigned long)N);
        return UINT64_MAX;
    }

    /* Invalidate DST — HP0 wrote to DDR, PS cache holds stale zeros */
    Xil_DCacheInvalidateRange((UINTPTR)DST_BASE, bytes);

    XTime_GetTime(&t1);
    pmu_read_all(pe);
    l2_read(le);
    /* ── End timed region ─────────────────────────────────────────────── */

    return ticks_to_us(t1 - t0);
}

/**
 * benchmark_noncoherent() — HP path, explicit flush + invalidate.
 *
 * SRC (SRC) is filled dirty then explicitly flushed to DDR.  Because the
 * ACP read port goes to DDR when cache lines are clean, this makes the DMA
 * read functionally equivalent to an HP0 read.  This matches the real-world
 * non-coherent discipline users must follow on the HP path.
 *
 * DST zeros are also flushed for the same reason as the ACP benchmark.
 *
 * Both SRC flush, DST flush, and DST invalidate are inside the timed window
 * so the chart reflects the true total PS cost of non-coherent transfers.
 *
 * Returns elapsed microseconds, or UINT64_MAX on DMA error/timeout.
 */
static uint64_t benchmark_noncoherent(uint32_t N,
                                       pmu_counts_t *ps, pmu_counts_t *pe,
                                       l2_counts_t  *ls, l2_counts_t  *le)
{
    uint32_t bytes = align_up(N * N * (uint32_t)sizeof(float));
    XTime t0, t1;

    /* ── Untimed setup ────────────────────────────────────────────────────
     * Fill SRC dirty — the flush is what we want to measure, so the
     * fill itself sits outside the timed window.
     * Clear DST here too; its flush happens inside the timed window.    */
    mat_fill(SRC, N, 2.0f);
    for (uint32_t i = 0U; i < N * N; ++i) DST[i] = 0.0f;

    DMB();

    /* ── Timed region ─────────────────────────────────────────────────── */
    pmu_reset_counters();
    l2_reset();
    pmu_read_all(ps);
    l2_read(ls);
    XTime_GetTime(&t0);

    /* 1. Flush SRC — push dirty lines to DDR so the DMA reads correct data.
     *    Omitting this is the canonical non-coherent bug (see smoke_test_stale). */
    Xil_DCacheFlushRange((UINTPTR)SRC_BASE, bytes);

    /* 2. Flush DST zeros — same writeback-race prevention as ACP path.  */
    Xil_DCacheFlushRange((UINTPTR)DST_BASE, bytes);

    /* 3. S2MM first */
    int rc = XAxiDma_SimpleTransfer(&dma,
                                    (UINTPTR)DST_BASE,
                                    bytes,
                                    XAXIDMA_DEVICE_TO_DMA);  /* S2MM */
    if (rc != XST_SUCCESS) {
        xil_printf("# ERROR: HP S2MM failed (N=%lu rc=%d)\r\n",
                   (unsigned long)N, rc);
        return UINT64_MAX;
    }

    /* 4. MM2S: cache clean → ACP read falls through to DDR */
    rc = XAxiDma_SimpleTransfer(&dma,
                                (UINTPTR)SRC_BASE,
                                bytes,
                                XAXIDMA_DMA_TO_DEVICE);      /* MM2S */
    if (rc != XST_SUCCESS) {
        xil_printf("# ERROR: HP MM2S failed (N=%lu rc=%d)\r\n",
                   (unsigned long)N, rc);
        return UINT64_MAX;
    }

    /* 5. Wait — reused from dma_smoke_test.c */
    if (dma_wait_done() != 0) {
        xil_printf("# WARNING: HP timeout (N=%lu)\r\n", (unsigned long)N);
        return UINT64_MAX;
    }

    /* 6. Invalidate DST */
    Xil_DCacheInvalidateRange((UINTPTR)DST_BASE, bytes);

    XTime_GetTime(&t1);
    pmu_read_all(pe);
    l2_read(le);
    /* ── End timed region ─────────────────────────────────────────────── */

    return ticks_to_us(t1 - t0);
}

/**
 * benchmark_sw_matmul() — pure-software N×N matrix multiply on the ARM CPU.
 *
 * This is the Phase-1 software baseline: the CPU computes C = A × B entirely
 * in software with no HLS accelerator involved.  DMA is NOT used here; the
 * comparison point is against benchmark_coherent / benchmark_noncoherent which
 * measure DMA data-movement cost without any compute.
 *
 * When the HLS accelerator is added (Phase 2), DMA will stream A and B to the
 * fabric and receive C back.  This function provides the reference latency that
 * the accelerator must beat.
 *
 * Memory layout:
 *   SRC (0x10000000)   = matrix A  (N×N floats)
 *   MAT_B (0x10400000) = matrix B  (N×N floats)
 *   DST (0x10800000)   = matrix C  (N×N floats, result)
 *
 * Cache discipline: A and B are flushed to DDR before timing starts so the
 * timed interval captures realistic DDR→cache cold-load cost, not just L1
 * hits from the fill.  DST is zeroed and flushed before the timed multiply.
 *
 * Inner loop uses the cache-friendly k-loop reordering (i-k-j) to improve
 * L1 hit rate on the B row access pattern.
 *
 * Returns elapsed microseconds, or UINT64_MAX on overflow.
 */
static uint64_t benchmark_sw_matmul(uint32_t N,
                                     pmu_counts_t *ps, pmu_counts_t *pe,
                                     l2_counts_t  *ls, l2_counts_t  *le)
{
    uint32_t bytes = align_up(N * N * (uint32_t)sizeof(float));
    XTime t0, t1;

    /* ── Untimed setup ────────────────────────────────────────────────────
     * Fill A and B with deterministic patterns, flush both to DDR so the
     * timed region starts with a cold cache (realistic DDR access cost).
     * Zero DST and flush it so no stale writeback can corrupt the result. */
    mat_fill(SRC,   N, 1.0f);
    mat_fill(MAT_B, N, 2.0f);
    Xil_DCacheFlushRange((UINTPTR)SRC_BASE,   bytes);
    Xil_DCacheFlushRange((UINTPTR)MAT_B_BASE, bytes);

    for (uint32_t i = 0U; i < N * N; ++i) DST[i] = 0.0f;
    Xil_DCacheFlushRange((UINTPTR)DST_BASE, bytes);

    DMB();

    /* ── Timed region ─────────────────────────────────────────────────── */
    pmu_reset_counters();
    l2_reset();
    pmu_read_all(ps);
    l2_read(ls);
    XTime_GetTime(&t0);

    /* C = A × B  (i-k-j order — cache-friendly B row reuse) */
    for (uint32_t i = 0U; i < N; ++i) {
        for (uint32_t k = 0U; k < N; ++k) {
            float a_ik = SRC[i * N + k];
            for (uint32_t j = 0U; j < N; ++j)
                DST[i * N + j] += a_ik * MAT_B[k * N + j];
        }
    }

    DSB();   /* ensure all stores to DST are globally visible */

    XTime_GetTime(&t1);
    pmu_read_all(pe);
    l2_read(le);
    /* ── End timed region ─────────────────────────────────────────────── */

    return ticks_to_us(t1 - t0);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * HLS ACCELERATOR BENCHMARK  (compiled only after platform rebuild with new XSA)
 * ───────────────────────────────────────────────────────────────────────────*/
#ifdef XPAR_MATMUL_0_DEVICE_ID
/**
 * benchmark_matmul() — Vitis HLS matmul_0 accelerator via AXI-Lite control.
 *
 * The accelerator (matmul_0) is a memory-mapped master; it does NOT connect to
 * the AXI DMA stream ports.  It accesses DDR independently:
 *   - Reads A and B via m_axi_ACP → axi_intercon_acp → S_AXI_ACP (coherent)
 *   - Writes C via m_axi_HP0 → axi_intercon_hp0 → S_AXI_HP0 (non-coherent)
 *
 * Cache protocol:
 *   A and B: flushed to DDR before start (ACP snoops, but flush ensures
 *            DDR consistency if lines were evicted between fill and start).
 *   C: invalidated after ap_done (HP0 wrote to DDR; PS cache holds stale zeros).
 *
 * Timed interval: XMatmul_Start() → ap_done poll clears → invalidate DST.
 *
 * Returns elapsed microseconds, or UINT64_MAX on error.
 */
static uint64_t benchmark_matmul(uint32_t N,
                                  pmu_counts_t *ps, pmu_counts_t *pe,
                                  l2_counts_t  *ls, l2_counts_t  *le)
{
    uint32_t bytes = align_up(N * N * (uint32_t)sizeof(float));
    XTime t0, t1;

    /* ── Untimed setup ────────────────────────────────────────────────────
     * Fill A and B, flush both to DDR so the accelerator reads coherent data.
     * Zero DST and flush its zeros to prevent a speculative PS writeback
     * corrupting the HP0 result after the accelerator has committed it.
     * Invalidate B_T scratch — HW writes via HP0 then reads via ACP;
     * without this the SCU may serve stale PS cache lines at that address. */
    mat_fill(SRC,   N, 1.0f);
    mat_fill(MAT_B, N, 2.0f);
    Xil_DCacheFlushRange((UINTPTR)SRC_BASE,   bytes);
    Xil_DCacheFlushRange((UINTPTR)MAT_B_BASE, bytes);

    for (uint32_t i = 0U; i < N * N; ++i) DST[i] = 0.0f;
    Xil_DCacheFlushRange((UINTPTR)DST_BASE,    bytes);
    Xil_DCacheInvalidateRange((UINTPTR)MAT_B_T_BASE, bytes);

    DMB();

    /* ── Timed region ─────────────────────────────────────────────────── */
    pmu_reset_counters();
    l2_reset();
    pmu_read_all(ps);
    l2_read(ls);
    XTime_GetTime(&t0);

    XMatmul_Set_A(&matmul_hw,   SRC_BASE);
    XMatmul_Set_B(&matmul_hw,   MAT_B_BASE);
    XMatmul_Set_B_T(&matmul_hw, MAT_B_T_BASE);   /* HW transposes B internally */
    XMatmul_Set_C(&matmul_hw,   DST_BASE);
    XMatmul_Set_N(&matmul_hw,   N);
    XMatmul_Start(&matmul_hw);

    while (!XMatmul_IsDone(&matmul_hw));   /* poll ap_done */

    /* Invalidate DST — HP0 wrote to DDR, PS cache holds stale zeros */
    Xil_DCacheInvalidateRange((UINTPTR)DST_BASE, N * N * sizeof(float));

    XTime_GetTime(&t1);
    pmu_read_all(pe);
    l2_read(le);
    /* ── End timed region ─────────────────────────────────────────────── */

    return ticks_to_us(t1 - t0);
}
#endif /* XPAR_MATMUL_0_DEVICE_ID */

/* ─────────────────────────────────────────────────────────────────────────────
 * PUBLIC ENTRY POINT
 * ───────────────────────────────────────────────────────────────────────────*/

/**
 * run_benchmark_sweep() — sweep all sizes × modes, print CSV over UART.
 *
 * Call this from main() after run_dma_smoke_tests() has passed — the smoke
 * tests initialise `dma` and confirm hardware is wired correctly.
 *
 * Modes run per size:
 *   ACP    — DMA loopback via coherent ACP port (no cache flush on SRC)
 *   HP     — DMA loopback via non-coherent HP0 port (explicit flush + invalidate)
 *   SW     — Software N×N matrix multiply on ARM CPU (Phase-1 baseline)
 *   MATMUL — Vitis HLS accelerator matmul_0 (Phase-2, when XSA is updated)
 *
 * Ordering: all REPEATS of each mode at N before moving to the next size.
 * This keeps cache state symmetric and comparable between modes at each size.
 *
 * Capture UART output and feed to:
 *   python3 analysis/plot_results.py results.log --out analysis/figures/
 */
void run_benchmark_sweep(void)
{
    pmu_counts_t ps, pe;
    l2_counts_t  ls, le;

    xil_printf("\r\n");
    xil_printf("# =====================================================\r\n");
#if MATMUL_MODE == MATMUL_HW
    xil_printf("# ACP vs HP0 vs HW-Matmul Benchmark — Zybo Z7-20\r\n");
#else
    xil_printf("# ACP vs HP0 vs SW-Matmul Benchmark — Zybo Z7-20\r\n");
#endif
    xil_printf("# COUNTS_PER_SEC=%lu  REPEATS=%lu\r\n",
               (unsigned long)COUNTS_PER_SECOND, (unsigned long)REPEATS);
    xil_printf("# mode,N,elapsed_us,l1d_miss,l1d_access,"
               "l2d_miss,l2d_access,l1_hit_pct,l2_hit_pct\r\n");
    xil_printf("# =====================================================\r\n");

    for (uint32_t si = 0U; si < NUM_SIZES; ++si) {
        uint32_t N = SIZES[si];

        for (uint32_t rep = 0U; rep < REPEATS; ++rep) {
            uint64_t us = benchmark_coherent(N, &ps, &pe, &ls, &le);
            if (us == UINT64_MAX)
                xil_printf("# SKIP ACP N=%lu rep=%lu\r\n",
                           (unsigned long)N, (unsigned long)rep);
            else
                print_csv_row("ACP", N, us, &ps, &pe, &ls, &le);
        }

        for (uint32_t rep = 0U; rep < REPEATS; ++rep) {
            uint64_t us = benchmark_noncoherent(N, &ps, &pe, &ls, &le);
            if (us == UINT64_MAX)
                xil_printf("# SKIP HP  N=%lu rep=%lu\r\n",
                           (unsigned long)N, (unsigned long)rep);
            else
                print_csv_row("HP", N, us, &ps, &pe, &ls, &le);
        }

#if MATMUL_MODE == MATMUL_SW
        for (uint32_t rep = 0U; rep < REPEATS; ++rep) {
            uint64_t us = benchmark_sw_matmul(N, &ps, &pe, &ls, &le);
            if (us == UINT64_MAX)
                xil_printf("# SKIP SW  N=%lu rep=%lu\r\n",
                           (unsigned long)N, (unsigned long)rep);
            else
                print_csv_row("SW", N, us, &ps, &pe, &ls, &le);
        }
#elif MATMUL_MODE == MATMUL_HW
        for (uint32_t rep = 0U; rep < REPEATS; ++rep) {
            uint64_t us = benchmark_matmul(N, &ps, &pe, &ls, &le);
            if (us == UINT64_MAX)
                xil_printf("# SKIP MATMUL N=%lu rep=%lu\r\n",
                           (unsigned long)N, (unsigned long)rep);
            else
                print_csv_row("MATMUL", N, us, &ps, &pe, &ls, &le);
        }
#endif
    }

    xil_printf("# Sweep complete.\r\n");
}
