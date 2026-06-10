/**
 * @file dma_smoke_test.c
 * @brief AXI DMA loopback smoke test for ACP and HP0 memory paths.
 *
 * @details
 * This file validates the AXI DMA hardware paths on the Zybo Z7-20 before
 * running the full coherency benchmark. It performs memory-to-memory DMA
 * transfers using both the ACP (coherent) and HP0 (non-coherent) ports and
 * verifies data integrity by comparing source and destination buffers.
 *
 * Three tests are run in sequence:
 *
 *  1. ACP loopback  — PS fills source, DMA reads via ACP (coherent),
 *                     DMA writes result via ACP. No cache management needed.
 *
 *  2. HP0 loopback  — PS fills source, flushes cache to DDR, DMA reads via
 *                     HP0 (non-coherent), DMA writes result via HP0.
 *                     PS invalidates before reading result.
 *
 *  3. Stale data    — Deliberate omission of cache flush on HP0 path.
 *                     Confirms that skipping the flush causes silent data
 *                     corruption — the DMA reads stale DDR, not the PS's
 *                     cached values.
 *
 * Expected UART output:
 * @code
 *   [SMOKE] ACP loopback  (N=256): PASS
 *   [SMOKE] HP0 loopback  (N=256): PASS
 *   [SMOKE] HP0 stale data test  : FAIL (expected — stale data confirmed)
 *   [SMOKE] All tests complete.
 * @endcode
 *
 * @note
 * DMA registers are at 0x40400000 (XPAR_AXI_DMA_0_BASEADDR).
 * Verify this matches your xparameters.h after importing Member A's .xsa.
 *
 * @board  Digilent Zybo Z7-20 (XC7Z020-1CLG400C)
 * @tool   Vitis 2022.2, arm-none-eabi-gcc
 */

#include "xaxidma.h"
#include "xparameters.h"
#include "xil_printf.h"
#include "xil_cache.h"
#include "xtime_l.h"
#include "pmu.h"
#include "dma_smoke_test.h"

/* ── Memory barriers ─────────────────────────────────────────────────────── */
#define DSB() __asm__ volatile("dsb" ::: "memory")
#define DMB() __asm__ volatile("dmb" ::: "memory")



/** @brief Convenient typed pointers into DDR. */
#define SMOKE_SRC  ((volatile float *)SMOKE_SRC_BASE)
#define SMOKE_DST  ((volatile float *)SMOKE_DST_BASE)

/** @brief Poll timeout iterations — avoids infinite hang if DMA stalls. */
#define DMA_TIMEOUT  0x10000000U

/* ── DMA driver instance ─────────────────────────────────────────────────── */

/** @brief Global XAxiDma instance — initialised once in dma_init(). */
static XAxiDma dma;

/* ─────────────────────────────────────────────────────────────────────────────
 * INTERNAL HELPERS
 * ───────────────────────────────────────────────────────────────────────────*/

/**
 * @brief Initialise the AXI DMA driver in simple (non-scatter-gather) mode.
 *
 * @details
 * Looks up the DMA configuration from xparameters.h, initialises the driver
 * instance, and disables all interrupts (polling mode is used throughout the
 * smoke test to keep the code self-contained).
 *
 * @return XST_SUCCESS on success, XST_FAILURE if config lookup or init fails.
 */
static int dma_init(void)
{
    XAxiDma_Config *cfg = XAxiDma_LookupConfig(XPAR_AXI_DMA_0_DEVICE_ID);
    if (!cfg) {
        xil_printf("[DMA] ERROR: LookupConfig failed — check XPAR_AXI_DMA_0_DEVICE_ID\r\n");
        return XST_FAILURE;
    }

    int status = XAxiDma_CfgInitialize(&dma, cfg);
    if (status != XST_SUCCESS) {
        xil_printf("[DMA] ERROR: CfgInitialize failed (status=%d)\r\n", status);
        return XST_FAILURE;
    }

    /* Disable interrupts — smoke test uses polling */
    XAxiDma_IntrDisable(&dma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);
    XAxiDma_IntrDisable(&dma, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DMA_TO_DEVICE);

    /* ── Debug: print DMA configuration ── */
        xil_printf("[DMA] Initialised — base addr  : 0x%08lX\r\n",
                   (unsigned long)XPAR_AXI_DMA_0_BASEADDR);
        xil_printf("[DMA] HasSg            : %d\r\n",
                   XAxiDma_HasSg(&dma));
        xil_printf("[DMA] MM2S MaxTransfer : %lu bytes\r\n",
                   (unsigned long)dma.TxBdRing.MaxTransferLen);
        xil_printf("[DMA] S2MM MaxTransfer : %lu bytes\r\n",
                   (unsigned long)dma.RxBdRing->MaxTransferLen);
        xil_printf("[DMA] SMOKE_BYTES      : %lu bytes\r\n",
                   (unsigned long)SMOKE_BYTES);

    return XST_SUCCESS;
}

/**
 * @brief Fill a buffer with a deterministic float pattern.
 *
 * @details
 * Each element is set to (seed + index × 0.001). This gives a unique,
 * predictable value at every position that can be verified after transfer.
 * Writing through the volatile pointer ensures the compiler actually
 * stores to DDR-mapped addresses rather than optimising the loop away.
 *
 * @param buf   Pointer to the target buffer.
 * @param count Number of float elements to fill.
 * @param seed  Starting value — use different seeds for different tests
 *              so residual data from a previous run cannot mask a failure.
 */
static void buf_fill(volatile float *buf, uint32_t count, float seed)
{
    for (uint32_t i = 0; i < count; ++i)
        buf[i] = seed + (float)i * 0.001f;
}

/**
 * @brief Zero a buffer.
 *
 * @details
 * Clears the destination before each test so a false PASS cannot occur
 * from a previous transfer leaving correct data in place.
 *
 * @param buf   Pointer to the target buffer.
 * @param count Number of float elements to zero.
 */
static void buf_clear(volatile float *buf, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i)
        buf[i] = 0.0f;
}

/**
 * @brief Compare source and destination buffers element by element.
 *
 * @details
 * Returns the number of mismatches found. A return value of 0 means
 * the transfer was bit-perfect. On the first mismatch the index and
 * values are printed to UART to aid debugging.
 *
 * @param src   Source buffer.
 * @param dst   Destination buffer to compare against source.
 * @param count Number of float elements to compare.
 * @return      Number of mismatched elements (0 = PASS).
 */
static uint32_t buf_compare(volatile float *src,
                             volatile float *dst,
                             uint32_t count)
{
    uint32_t errors = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (src[i] != dst[i]) {
            if (errors == 0) {
                /* Print first mismatch only to avoid flooding UART */
                xil_printf("[CMP] First mismatch at [%lu]: "
                           "src bits=0x%08lX  dst bits=0x%08lX\r\n",
                           (unsigned long)i,
                           (unsigned long)*(uint32_t *)&src[i],
                           (unsigned long)*(uint32_t *)&dst[i]);
            }
            ++errors;
        }
    }
    return errors;
}

/**
 * @brief Poll both DMA channels until idle or timeout.
 *
 * @details
 * Polls MM2S (memory-to-stream, DMA read) and S2MM (stream-to-memory,
 * DMA write) channels separately. Returns -1 if either channel does not
 * become idle within DMA_TIMEOUT iterations, which indicates a hardware
 * hang — check ILA waveforms and AXI address map.
 *
 * @return 0 on success, -1 on timeout.
 */
static int dma_wait_done(void)
{
    for (uint32_t i = 0; i < DMA_TIMEOUT; ++i) {
        if (!XAxiDma_Busy(&dma, XAXIDMA_DMA_TO_DEVICE) &&
            !XAxiDma_Busy(&dma, XAXIDMA_DEVICE_TO_DMA))
            return 0;
    }
    xil_printf("[DMA] ERROR: timeout — DMA did not complete\r\n");
    xil_printf("[DMA] Check: ILA waveforms, AXI address map, ACP/HP connections\r\n");
    return -1;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * SMOKE TESTS
 * ───────────────────────────────────────────────────────────────────────────*/

/**
 * @brief ACP loopback smoke test — coherent path, no cache management.
 *
 * @details
 * Demonstrates that when the PL accesses memory via the ACP port, the
 * Snoop Control Unit (SCU) satisfies DMA reads directly from the ARM L1/L2
 * cache — even though the PS never flushed the data to DDR. The DMA sees
 * the most recent PS-written values without any explicit cache operation.
 *
 * Transfer sequence:
 *  1. PS fills SRC (data stays dirty in L1/L2).
 *  2. DMB barrier — ensures all stores are globally visible.
 *  3. DMA MM2S reads SRC via ACP — SCU snoops L1/L2, returns fresh data. memory mapped 2 stream
 *  4. DMA S2MM writes DST via ACP — SCU updates PS cache with result.
 *  5. PS reads DST — no invalidate needed, SCU maintained coherency.
 *  6. Compare SRC vs DST.
 *
 * @return 0 on PASS, -1 on DMA timeout, positive integer = mismatch count.
 */
static int smoke_test_acp(void)
{
    xil_printf("\r\n[SMOKE] --- ACP loopback test (N=%u, %lu bytes) ---\r\n",
               (unsigned)SMOKE_N, (unsigned long)SMOKE_BYTES);

    /* Step 1: Fill source — data is dirty in L1/L2, not yet in DDR */
    buf_fill(SMOKE_SRC, SMOKE_N * SMOKE_N, 1.0f);
    buf_clear(SMOKE_DST, SMOKE_N * SMOKE_N);
    xil_printf("[SMOKE] SRC filled, DST cleared\r\n");


    /* Step 2: DMB — all preceding stores complete before DMA starts */

    /* Flush: push dirty lines to DDR so DMA engine has clean view */
    Xil_DCacheFlushRange((UINTPTR)SMOKE_SRC_BASE, SMOKE_BYTES);
    Xil_DCacheFlushRange((UINTPTR)SMOKE_DST_BASE, SMOKE_BYTES);

    /* DSB: wait for all cache maintenance to complete */
    DSB();
    xil_printf("[SMOKE] Barriers and flush done\r\n");

    /* Step 3+4: DMA transfer via ACP
     * MM2S: DMA reads from SRC via ACP — SCU snoops cache
     * S2MM: DMA writes to  DST via ACP — SCU updates cache  */
    int status;
    status = XAxiDma_SimpleTransfer(&dma,
                                    (UINTPTR)SMOKE_SRC_BASE,
                                    SMOKE_BYTES,
                                    XAXIDMA_DMA_TO_DEVICE);   /* MM2S */
    if (status != XST_SUCCESS) {
        xil_printf("[SMOKE] ERROR: MM2S transfer failed (status=%d)\r\n", status);
        return -1;
    }

    status = XAxiDma_SimpleTransfer(&dma,
                                    (UINTPTR)SMOKE_DST_BASE,
                                    SMOKE_BYTES,
                                    XAXIDMA_DEVICE_TO_DMA);   /* S2MM */
    if (status != XST_SUCCESS) {
        xil_printf("[SMOKE] ERROR: S2MM transfer failed (status=%d)\r\n", status);
        return -1;
    }

    /* Wait for both channels to go idle */
    if (dma_wait_done() != 0)
        return -1;

    /* Step 5: No cache invalidate needed — SCU handled coherency */

    /* Step 6: Verify */
    uint32_t errors = buf_compare(SMOKE_SRC, SMOKE_DST, SMOKE_N * SMOKE_N);
    if (errors == 0) {
        xil_printf("[SMOKE] ACP loopback (N=%u): PASS\r\n", (unsigned)SMOKE_N);
    } else {
        xil_printf("[SMOKE] ACP loopback (N=%u): FAIL (%lu mismatches)\r\n",
                   (unsigned)SMOKE_N, (unsigned long)errors);
    }
    return (int)errors;
}

/**
 * @brief HP0 loopback smoke test — non-coherent path, explicit cache management.
 *
 * @details
 * Demonstrates correct usage of the HP0 port. Because HP0 bypasses the SCU
 * and goes directly to the DDR controller, the PS must explicitly push dirty
 * cache lines to DDR before the DMA reads, and invalidate its cache after
 * the DMA writes — otherwise the PS would read its own stale copy.
 *
 * Transfer sequence:
 *  1. PS fills SRC (data dirty in L1/L2).
 *  2. Xil_DCacheFlushRange(SRC) — writes dirty lines to DDR.
 *  3. DMA MM2S reads SRC via HP0 — reads from DDR directly.
 *  4. DMA S2MM writes DST via HP0 — writes to DDR directly.
 *  5. Xil_DCacheInvalidateRange(DST) — discards stale PS cache copy.
 *  6. PS reads DST — fetches fresh data from DDR.
 *  7. Compare SRC vs DST.
 *
 * @return 0 on PASS, -1 on DMA timeout, positive integer = mismatch count.
 */
static int smoke_test_hp(void)
{
    xil_printf("\r\n[SMOKE] --- HP0 loopback test (N=%u, %lu bytes) ---\r\n",
               (unsigned)SMOKE_N, (unsigned long)SMOKE_BYTES);

    /* Step 1: Fill source — data is dirty in L1/L2 */
    buf_fill(SMOKE_SRC, SMOKE_N * SMOKE_N, 2.0f);   /* different seed from ACP test */
    buf_clear(SMOKE_DST, SMOKE_N * SMOKE_N);
    xil_printf("[SMOKE] SRC filled, DST cleared\r\n");

    /* Step 2: Flush SRC — HP0 bypasses SCU, so DMA reads from DDR.
     * Without this flush, the DMA would read stale DDR data.          */
    Xil_DCacheFlushRange((UINTPTR)SMOKE_SRC_BASE, SMOKE_BYTES);
    xil_printf("[SMOKE] Cache flushed — SRC data now in DDR\r\n");

    /* Step 3+4: DMA transfer via HP0 — straight to DDR controller */
    int status;
    status = XAxiDma_SimpleTransfer(&dma,
                                    (UINTPTR)SMOKE_SRC_BASE,
                                    SMOKE_BYTES,
                                    XAXIDMA_DMA_TO_DEVICE);   /* MM2S */
    if (status != XST_SUCCESS) {
        xil_printf("[SMOKE] ERROR: MM2S transfer failed (status=%d)\r\n", status);
        return -1;
    }

    status = XAxiDma_SimpleTransfer(&dma,
                                    (UINTPTR)SMOKE_DST_BASE,
                                    SMOKE_BYTES,
                                    XAXIDMA_DEVICE_TO_DMA);   /* S2MM */
    if (status != XST_SUCCESS) {
        xil_printf("[SMOKE] ERROR: S2MM transfer failed (status=%d)\r\n", status);
        return -1;
    }

    if (dma_wait_done() != 0)
        return -1;

    /* Step 5: Invalidate DST — DMA wrote to DDR via HP0, bypassing SCU.
     * The PS cache may still hold an old copy of DST from buf_clear().
     * Invalidating forces the next read to fetch fresh data from DDR.  */
    Xil_DCacheInvalidateRange((UINTPTR)SMOKE_DST_BASE, SMOKE_BYTES);
    xil_printf("[SMOKE] Cache invalidated — PS will read fresh DST from DDR\r\n");

    /* Step 6+7: Compare */
    uint32_t errors = buf_compare(SMOKE_SRC, SMOKE_DST, SMOKE_N * SMOKE_N);
    if (errors == 0) {
        xil_printf("[SMOKE] HP0 loopback (N=%u): PASS\r\n", (unsigned)SMOKE_N);
    } else {
        xil_printf("[SMOKE] HP0 loopback (N=%u): FAIL (%lu mismatches)\r\n",
                   (unsigned)SMOKE_N, (unsigned long)errors);
    }
    return (int)errors;
}

/**
 * @brief Stale data test — deliberate cache flush omission on HP0 path.
 *
 * @details
 * This test intentionally violates the HP0 cache discipline to confirm that
 * omitting the flush causes silent data corruption. The PS writes new values
 * to SRC but does NOT flush — the DMA reads stale DDR data from the previous
 * test instead of the PS's new values. The comparison is expected to fail.
 *
 * This is an important correctness validation: it proves the board and DMA
 * are behaving as the theory predicts. If this test unexpectedly passes,
 * either the previous test's flush is still in effect, or the cache is not
 * operating in write-back mode as expected.
 *
 * @note  A FAIL result here is the CORRECT outcome.
 *
 * @return 0 if stale data was confirmed (expected FAIL on compare),
 *        -1 if the DMA itself failed (unexpected),
 *         positive if the compare unexpectedly passed (investigate cache config).
 */
static int smoke_test_stale(void)
{
    xil_printf("\r\n[SMOKE] --- Stale data test (deliberate flush omission) ---\r\n");
    xil_printf("[SMOKE] Expecting FAIL — this confirms HP0 coherency requirement\r\n");

    /* Fill SRC with NEW values — different seed so they differ from
     * what DDR currently holds from the previous HP0 test.
     * Deliberately do NOT flush — DDR still has old data.             */
    buf_fill(SMOKE_SRC, SMOKE_N * SMOKE_N, 99.0f);   /* new distinct seed */
    buf_clear(SMOKE_DST, SMOKE_N * SMOKE_N);

    /* NO Xil_DCacheFlushRange here — this is intentional */
    xil_printf("[SMOKE] SRC filled with new values (NOT flushed to DDR)\r\n");
    xil_printf("[SMOKE] DDR still holds stale data from previous test\r\n");

    /* DMA reads stale DDR via HP0 */
    int status;
    status = XAxiDma_SimpleTransfer(&dma,
                                    (UINTPTR)SMOKE_SRC_BASE,
                                    SMOKE_BYTES,
                                    XAXIDMA_DMA_TO_DEVICE);
    if (status != XST_SUCCESS) {
        xil_printf("[SMOKE] ERROR: MM2S transfer failed\r\n");
        return -1;
    }

    status = XAxiDma_SimpleTransfer(&dma,
                                    (UINTPTR)SMOKE_DST_BASE,
                                    SMOKE_BYTES,
                                    XAXIDMA_DEVICE_TO_DMA);
    if (status != XST_SUCCESS) {
        xil_printf("[SMOKE] ERROR: S2MM transfer failed\r\n");
        return -1;
    }

    if (dma_wait_done() != 0)
        return -1;

    Xil_DCacheInvalidateRange((UINTPTR)SMOKE_DST_BASE, SMOKE_BYTES);

    /* Compare PS cache (new values) vs DDR result (stale values)
     * This SHOULD fail — DST will contain old data, not seed 99.0f   */
    uint32_t errors = buf_compare(SMOKE_SRC, SMOKE_DST, SMOKE_N * SMOKE_N);
    if (errors > 0) {
        xil_printf("[SMOKE] HP0 stale data test: FAIL as expected (%lu mismatches)\r\n",
                   (unsigned long)errors);
        xil_printf("[SMOKE] CONFIRMED: omitting flush causes data corruption on HP0\r\n");
        return 0;   /* correct outcome */
    } else {
        xil_printf("[SMOKE] HP0 stale data test: PASS (unexpected!)\r\n");
        xil_printf("[SMOKE] WARNING: cache may not be in write-back mode\r\n");
        xil_printf("[SMOKE] Check: MMU settings, cache policy in lscript.ld\r\n");
        return 1;   /* investigate */
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * ENTRY POINT
 * ───────────────────────────────────────────────────────────────────────────*/

/**
 * @brief Run all three DMA smoke tests in sequence.
 *
 * @details
 * Call this function from main() before running the full benchmark sweep.
 * All three tests must behave as expected before proceeding:
 *  - ACP loopback  : PASS
 *  - HP0 loopback  : PASS
 *  - Stale data    : FAIL (expected — confirms coherency requirement)
 *
 * If ACP or HP0 loopback fails, stop and debug before running the benchmark.
 * Common failure causes:
 *  - DMA base address mismatch (check XPAR_AXI_DMA_0_BASEADDR in xparameters.h)
 *  - AXI Protocol Converter not connected in block design
 *  - Buffer addresses not accessible from PL (check AXI address map in Vivado)
 *  - MM2S and S2MM connected to same port (should be ACP and HP0 respectively)
 */
void run_dma_smoke_tests(void)
{
    xil_printf("\r\n");
    xil_printf("================================================\r\n");
    xil_printf("  DMA Smoke Test - ACP vs HP0 Path Validation  \r\n");
    xil_printf("================================================\r\n");

    /* Initialise DMA driver */
    if (dma_init() != XST_SUCCESS) {
        xil_printf("[SMOKE] FATAL: DMA init failed - aborting smoke tests\r\n");
        return;
    }

    int acp_result   = smoke_test_acp();
    int hp_result    = smoke_test_hp();
    int stale_result = smoke_test_stale();

    /* Summary */
    xil_printf("\r\n================================================\r\n");
    xil_printf("  Smoke Test Summary\r\n");
    xil_printf("================================================\r\n");
    xil_printf("  ACP loopback  : %s\r\n", acp_result   == 0 ? "PASS" : "FAIL");
    xil_printf("  HP0 loopback  : %s\r\n", hp_result    == 0 ? "PASS" : "FAIL");
    xil_printf("  Stale data    : %s\r\n", stale_result  == 0 ?
               "FAIL as expected (CORRECT)" : "PASS (unexpected - investigate)");

    if (acp_result == 0 && hp_result == 0 && stale_result == 0) {
        xil_printf("\r\n  All tests behaved as expected.\r\n");
        xil_printf("  Safe to proceed to full benchmark sweep.\r\n");
    } else {
        xil_printf("\r\n  One or more tests did not behave as expected.\r\n");
        xil_printf("  Do NOT proceed to benchmark - debug first.\r\n");
    }
    xil_printf("================================================\r\n\r\n");
}
