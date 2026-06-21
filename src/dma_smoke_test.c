/**
 * @file   dma_smoke_test.c
 * @author Brunda Marpadaga (brundamarpadaga@gmail.com)
 * @author Bhavana Marpadaga (marapadagabhavana@gmail.com)
 * @brief  AXI DMA loopback smoke test for ACP and HP0 memory paths.
 *
 * @details
 * Validates the AXI DMA hardware paths on the Zybo Z7-20 before the full
 * coherency benchmark. Three tests are run in sequence:
 *
 *  1. ACP read path  — PS fills SRC (data dirty in L1/L2, NOT flushed to DDR).
 *                      DMA MM2S reads SRC via ACP — SCU snoops L1/L2 cache.
 *                      DMA S2MM writes DST via HP0 (hardwired in block design).
 *                      PS invalidates DST before reading (HP0 bypassed SCU).
 *                      Proves: SCU serves dirty cache lines to DMA without flush.
 *
 *  2. HP0 read path  — PS fills SRC and flushes to DDR before DMA reads.
 *                      DMA MM2S reads SRC via ACP (but DDR = cache after flush).
 *                      DMA S2MM writes DST via HP0.
 *                      PS invalidates DST before reading.
 *                      Proves: explicit flush + invalidate discipline works.
 *
 *  3. Stale data     — PS fills SRC with new values but deliberately omits flush.
 *                      DMA MM2S reads SRC via ACP — reads stale DDR values.
 *                      DMA S2MM writes stale data to DST via HP0.
 *                      Compare expected (new cache values) vs actual (stale DDR).
 *                      Proves: omitting flush on the read path causes corruption.
 *
 * @note
 * Hardware port mapping (Zybo Z7-20 block design):
 *   MM2S memory port → AXI Protocol Converter → S_AXI_ACP  (DMA reads)
 *   S2MM memory port → AXI Protocol Converter → S_AXI_HP0  (DMA writes)
 * S2MM always writes via HP0 regardless of which test is running.
 * DST must always be invalidated after transfer before PS reads it.
 *
 * S2MM must be armed before MM2S — MM2S pushes data onto the AXI stream
 * immediately and stalls if S2MM is not already waiting to receive.
 *
 * @note Portions of this code were generated with assistance from Claude AI (Anthropic).
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

/** @brief Global XAxiDma instance — initialised once in dma_init().
 *  Non-static so benchmark.c can share it via extern declaration in
 *  dma_smoke_test.h.  Only dma_init() (called from run_dma_smoke_tests)
 *  must ever call XAxiDma_CfgInitialize on it.*/

XAxiDma dma;

/* ─────────────────────────────────────────────────────────────────────────────
 * INTERNAL HELPERS
 * ───────────────────────────────────────────────────────────────────────────*/

/**
 * @brief Initialise the AXI DMA driver in simple (non-scatter-gather) mode.
 *
 * @details
 * Looks up the DMA configuration from xparameters.h using the device ID
 * defined by the .xsa export, initialises the driver instance, and disables
 * all interrupts. Polling mode is used throughout to keep the smoke test
 * self-contained without requiring an interrupt controller setup.
 *
 * Prints DMA configuration to UART on success for verification:
 * base address, scatter-gather mode, MM2S/S2MM max transfer lengths,
 * and the SMOKE_BYTES value that will be used in each test.
 *
 * @return XST_SUCCESS on success.
 * @return XST_FAILURE if the device ID is not found in xparameters.h
 *         or if driver initialisation fails — check that the .xsa was
 *         correctly imported and the platform was rebuilt after .xsa update.
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
        xil_printf("[DMA] Initialised - base addr  : 0x%08lX\r\n",
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
 * Each element is set to (seed + index × 0.001). Using a unique seed per
 * test ensures residual data from a previous transfer cannot mask a failure
 * by accidentally matching the new expected values.
 *
 * The volatile pointer prevents the compiler from optimising the loop away,
 * ensuring every store reaches the cache and eventually DDR.
 *
 * @param buf    Pointer to the target buffer in DDR.
 * @param count  Number of float elements to fill.
 * @param seed   Starting value. Use distinct seeds across tests:
 *               ACP test = 1.0f, HP0 test = 2.0f, stale test = 99.0f.
 */
static void buf_fill(volatile float *buf, uint32_t count, float seed)
{
    for (uint32_t i = 0; i < count; ++i)
        buf[i] = seed + (float)i * 0.001f;
}

/**
 * @brief Zero a destination buffer before each test.
 *
 * @details
 * Clears DST to 0.0f before each transfer so a false PASS cannot occur
 * from a previous test leaving correct data in place. The zeros also serve
 * as a recognisable sentinel — if DST reads back 0x00000000 after a transfer,
 * the PS cache was not invalidated and is returning the cached zeros rather
 * than the DMA result.
 *
 * @param buf    Pointer to the target buffer in DDR.
 * @param count  Number of float elements to zero.
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
 * On the first mismatch, prints the element index and both values in hex
 * to UART. Hex is used rather than float to avoid ambiguity from floating
 * point printing — 0x3F800000 is unambiguously 1.0f, while printf %f may
 * round or truncate. Only the first mismatch is printed to avoid flooding
 * the UART on large failure counts.
 *
 * @param src    Source buffer — the reference values written by buf_fill().
 * @param dst    Destination buffer — the values written by the DMA.
 * @param count  Number of float elements to compare.
 *
 * @return 0 if all elements match (transfer was bit-perfect).
 * @return Positive integer indicating the number of mismatched elements.
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
 * Polls MM2S (memory-to-stream) and S2MM (stream-to-memory) channels
 * simultaneously in a single loop. Both must be idle before the function
 * returns success — a transfer is not complete until the S2MM write has
 * fully committed to DDR.
 *
 * If timeout occurs, prints a diagnostic message directing the user to
 * check ILA waveforms. Common causes of timeout:
 *   - AXI stream loopback not connected in block design
 *   - S2MM not armed before MM2S (stream stall with no receiver)
 *   - AXI address map error (DMA accessing unmapped region)
 *   - Protocol converter misconfiguration
 *
 * @return  0 on success (both channels idle).
 * @return -1 on timeout (DMA did not complete within DMA_TIMEOUT iterations).
 */
int dma_wait_done(void)
{
    /* S2MM Status Register offset 0x34 — AXI DMA PG021 Table 2-21
     * Bit[1] = Idle: set when S2MM channel has completed and all
     * write data has been committed through the AXI interconnect.    */
    #define DMA_MM2S_SR  (XPAR_AXI_DMA_0_BASEADDR + 0x04U)
    #define DMA_S2MM_SR  (XPAR_AXI_DMA_0_BASEADDR + 0x34U)
    #define DMA_IDLE_BIT (1U << 1)

    /* Step 1: Wait for MM2S Idle */
    for (uint32_t i = 0; i < DMA_TIMEOUT; ++i) {
        if (Xil_In32(DMA_MM2S_SR) & DMA_IDLE_BIT)
            break;
        if (i == DMA_TIMEOUT - 1) {
            xil_printf("[DMA] ERROR: MM2S idle timeout\r\n");
            return -1;
        }
    }

    /* Step 2: Wait for S2MM Idle — only set after all writes
     * drain through the AXI write buffer into DDR               */
    for (uint32_t i = 0; i < DMA_TIMEOUT; ++i) {
        if (Xil_In32(DMA_S2MM_SR) & DMA_IDLE_BIT)
            break;
        if (i == DMA_TIMEOUT - 1) {
            xil_printf("[DMA] ERROR: S2MM idle timeout\r\n");
            return -1;
        }
    }

    /* Step 3: DSB — ARM memory barrier ensures all AXI writes
     * visible to PS before invalidate and compare               */
    __asm__ volatile("dsb" ::: "memory");

    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * SMOKE TESTS
 * ───────────────────────────────────────────────────────────────────────────*/

/**
 * @brief ACP read path smoke test — SCU cache snooping validation.
 *
 * @details
 * Validates that the ACP port correctly participates in ARM cache coherency.
 * The PS fills SRC without flushing to DDR — data stays dirty in L1/L2.
 * The DMA reads SRC via ACP; the SCU snoops the PS cache and serves the
 * dirty cache lines directly, so DDR is never read for the source data.
 *
 * Hardware port mapping for this test:
 *   MM2S reads  SRC via ACP  — SCU snoops L1/L2, returns cached values
 *   S2MM writes DST via HP0  — writes directly to DDR, bypasses SCU
 *
 * Because S2MM always uses HP0, DST must be invalidated after transfer.
 * The PS cache still holds buf_clear() zeros for DST — invalidating forces
 * the next read to fetch the DMA result from DDR.
 *
 * Transfer sequence:
 *  1. buf_fill(SRC, seed=1.0f)  — data dirty in L1/L2, NOT in DDR
 *  2. buf_clear(DST)            — zeros in PS cache and DDR
 *  3. DMB + DSB barriers        — stores globally visible before DMA starts
 *  4. S2MM armed at DST         — receiver ready before MM2S sends
 *  5. MM2S started at SRC       — SCU snoops, serves 1.0f from cache
 *  6. dma_wait_done()           — both channels idle
 *  7. InvalidateRange(DST)      — discard cached zeros, force DDR read
 *  8. buf_compare(SRC, DST)     — SRC from cache (1.0f) vs DST from DDR
 *
 * @return  0 on PASS (all elements match).
 * @return -1 on DMA timeout or transfer error.
 * @return Positive integer indicating mismatch count on data error.
 */
static int smoke_test_acp(void)
{
    xil_printf("\r\n[SMOKE] --- ACP loopback test (N=%u, %lu bytes) ---\r\n",
               (unsigned)SMOKE_N, (unsigned long)SMOKE_BYTES);

    /* Step 1: Fill source — data is dirty in L1/L2, not yet in DDR */
    buf_fill(SMOKE_SRC, SMOKE_N * SMOKE_N, 1.0f);
    buf_clear(SMOKE_DST, SMOKE_N * SMOKE_N);
    xil_printf("[SMOKE] SRC filled, DST cleared\r\n");

    /* Flush DST zeros to DDR — prevents cache writeback from
     * overwriting DMA result after S2MM writes to DDR via HP0 */
    Xil_DCacheFlushRange((UINTPTR)SMOKE_DST_BASE, SMOKE_BYTES);

    /* ── Snooping verification ───────────────────────────────────────────
     * Goal: prove SRC DDR has stale values while SRC cache has fresh 1.0f.
     * If DMA still passes after this, SCU must be snooping cache.
     *
     * Step A: Read what DDR currently holds for SRC (bypass cache)      */
    Xil_DCacheInvalidateRange((UINTPTR)SMOKE_SRC_BASE, SMOKE_BYTES);
    volatile float *src_ptr = (volatile float *)SMOKE_SRC_BASE;
    float ddr_val = src_ptr[0];   /* cache miss → fetches from DDR */
    xil_printf("[VERIFY] SRC[0] from DDR   = 0x%08lX\r\n",
               (unsigned long)*(uint32_t*)&ddr_val);
    /* Expected: NOT 0x3F800000 (1.0f) — DDR is stale               */

    /* Step B: Re-fill SRC into cache — DDR still has stale value    */
    buf_fill(SMOKE_SRC, SMOKE_N * SMOKE_N, 1.0f);
    float cache_val = src_ptr[0]; /* cache hit → gets fresh 1.0f   */
    xil_printf("[VERIFY] SRC[0] from cache = 0x%08lX\r\n",
               (unsigned long)*(uint32_t*)&cache_val);
    /* Expected: 0x3F800000 (1.0f) — cache has fresh value           */

    xil_printf("[VERIFY] If DDR != 1.0f but test passes → SCU snooping confirmed\r\n");
    xil_printf("[VERIFY] If DDR == 1.0f → eviction occurred, result inconclusive\r\n");
    /* ── End verification ─────────────────────────────────────────── */

    /* Step 2: DMB — all preceding stores complete before DMA starts */
    DMB();

    /* DSB: wait for all cache maintenance to complete */
    DSB();
    xil_printf("[SMOKE] Barriers done - no flush, SCU should snoop cache\r\n");

    /* Step 3+4: DMA transfer via ACP
     * MM2S: DMA reads from SRC via ACP — SCU snoops cache
     * S2MM: DMA writes to  DST via HP0 — SCU updates cache  */
    int status;
    status = XAxiDma_SimpleTransfer(&dma,
                                        (UINTPTR)SMOKE_DST_BASE,
                                        SMOKE_BYTES,
                                        XAXIDMA_DEVICE_TO_DMA);   /* S2MM */
    if (status != XST_SUCCESS) {
            xil_printf("[SMOKE] ERROR: S2MM transfer failed (status=%d)\r\n", status);
            return -1;
    }
    status = XAxiDma_SimpleTransfer(&dma,
                                    (UINTPTR)SMOKE_SRC_BASE,
                                    SMOKE_BYTES,
                                    XAXIDMA_DMA_TO_DEVICE);   /* MM2S */
    if (status != XST_SUCCESS) {
        xil_printf("[SMOKE] ERROR: MM2S transfer failed (status=%d)\r\n", status);
        return -1;
    }



    /* Wait for both channels to go idle */
    if (dma_wait_done() != 0)
        return -1;



    /* Step 5: Invalidate DST — DMA wrote result via ACP to DDR.
         * PS cache still holds the buf_clear() zeros for DST addresses.
         * Invalidate so the compare reads fresh data from DDR.          */
    Xil_DCacheInvalidateRange((UINTPTR)SMOKE_DST_BASE, SMOKE_BYTES);

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
 * @brief HP0 read path smoke test — explicit cache flush discipline.
 *
 * @details
 * Validates the non-coherent transfer discipline. The PS fills SRC and
 * explicitly flushes dirty cache lines to DDR before the DMA reads — because
 * the ACP read path goes to DDR when cache is clean, this effectively tests
 * the DDR read path.
 *
 * Hardware port mapping for this test:
 *   MM2S reads  SRC via ACP  — cache clean after flush, so reads from DDR
 *   S2MM writes DST via HP0  — writes directly to DDR, bypasses SCU
 *
 * Transfer sequence:
 *  1. buf_fill(SRC, seed=2.0f)      — data dirty in L1/L2
 *  2. buf_clear(DST)                — zeros in cache and DDR
 *  3. FlushRange(SRC)               — push 2.0f to DDR
 *  4. S2MM armed at DST             — receiver ready
 *  5. MM2S started at SRC           — reads 2.0f from DDR
 *  6. dma_wait_done()
 *  7. InvalidateRange(DST)          — discard cached zeros
 *  8. buf_compare(SRC, DST)         — SRC from cache (2.0f) vs DST from DDR
 *
 * @return  0 on PASS.
 * @return -1 on DMA timeout or transfer error.
 * @return Positive integer = mismatch count.
 */
static int smoke_test_hp(void)
{
    xil_printf("\r\n[SMOKE] --- HP0 loopback test (N=%u, %lu bytes) ---\r\n",
               (unsigned)SMOKE_N, (unsigned long)SMOKE_BYTES);

    /* Step 1: Fill source — data is dirty in L1/L2 */
    buf_fill(SMOKE_SRC, SMOKE_N * SMOKE_N, 2.0f);   /* different seed from ACP test */
    buf_clear(SMOKE_DST, SMOKE_N * SMOKE_N);
    xil_printf("[SMOKE] SRC filled, DST cleared\r\n");

    /* Flush DST zeros to DDR — prevents cache writeback from
     * overwriting DMA result after S2MM writes to DDR via HP0 */
    Xil_DCacheFlushRange((UINTPTR)SMOKE_DST_BASE, SMOKE_BYTES);

    /* Step 2: Flush SRC — HP0 bypasses SCU, so DMA reads from DDR.
     * Without this flush, the DMA would read stale DDR data.          */
    Xil_DCacheFlushRange((UINTPTR)SMOKE_SRC_BASE, SMOKE_BYTES);
    xil_printf("[SMOKE] Cache flushed - SRC data now in DDR\r\n");

    /* Step 3+4: DMA transfer via HP0 — straight to DDR controller */
    int status;


    status = XAxiDma_SimpleTransfer(&dma,
                                    (UINTPTR)SMOKE_DST_BASE,
                                    SMOKE_BYTES,
                                    XAXIDMA_DEVICE_TO_DMA);   /* S2MM */
    if (status != XST_SUCCESS) {
        xil_printf("[SMOKE] ERROR: S2MM transfer failed (status=%d)\r\n", status);
        return -1;
    }

    status = XAxiDma_SimpleTransfer(&dma,
                                        (UINTPTR)SMOKE_SRC_BASE,
                                        SMOKE_BYTES,
                                        XAXIDMA_DMA_TO_DEVICE);   /* MM2S */
    if (status != XST_SUCCESS) {
        xil_printf("[SMOKE] ERROR: MM2S transfer failed (status=%d)\r\n", status);
        return -1;
    }

    if (dma_wait_done() != 0)
        return -1;

    /* Step 5: Invalidate DST — DMA wrote to DDR via HP0, bypassing SCU.
     * The PS cache may still hold an old copy of DST from buf_clear().
     * Invalidating forces the next read to fetch fresh data from DDR.  */
    Xil_DCacheInvalidateRange((UINTPTR)SMOKE_DST_BASE, SMOKE_BYTES);
    xil_printf("[SMOKE] Cache invalidated - PS will read fresh DST from DDR\r\n");

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
 * @brief Stale data test — deliberate DST flush omission.
 *
 * @details
 * Proves that omitting Xil_DCacheFlushRange(DST) before arming S2MM
 * allows cache writeback to overwrite the DMA result in DDR, causing
 * silent data corruption on the write side.
 *
 * The PS fills DST with zeros via buf_clear() — those zero lines sit
 * dirty in L1/L2 cache. The DMA S2MM writes the correct result to DDR
 * via HP0, bypassing the SCU. The CPU cache then writes its dirty zeros
 * back to DDR at an unpredictable time, overwriting the DMA result.
 * When the PS invalidates and reads DST, it fetches the zeros from DDR
 * rather than the DMA result — corruption confirmed.
 *
 * This is a write-side coherency failure. Read-side staleness cannot
 * be demonstrated while MM2S is wired to ACP — the SCU always snoops
 * the cache regardless of whether SRC was flushed to DDR.
 *
 * A FAIL result here is the CORRECT and expected outcome.
 * If this test unexpectedly passes, the dirty zeros were not written
 * back to DDR before the invalidate — the cache may not be operating
 * in write-back mode, or the timing of the writeback varied. Investigate
 * MMU settings and cache policy in lscript.ld.
 *
 * @return  0 if corruption confirmed (compare failed as expected).
 * @return -1 if DMA transfer itself failed unexpectedly.
 * @return Positive integer if compare unexpectedly passed — investigate.
 */
static int smoke_test_stale(void)
{
    xil_printf("\r\n[SMOKE] --- Stale data test (deliberate flush omission) ---\r\n");
    xil_printf("[SMOKE] Expecting FAIL - this confirms HP0 coherency requirement\r\n");

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
                                    (UINTPTR)SMOKE_DST_BASE,
                                    SMOKE_BYTES,
                                    XAXIDMA_DEVICE_TO_DMA); /* S2MM */
    if (status != XST_SUCCESS) {
        xil_printf("[SMOKE] ERROR: S2MM transfer failed\r\n");
        return -1;
    }

    status = XAxiDma_SimpleTransfer(&dma,
                                        (UINTPTR)SMOKE_SRC_BASE,
                                        SMOKE_BYTES,
                                        XAXIDMA_DMA_TO_DEVICE); /* MM2S */
    if (status != XST_SUCCESS) {
         xil_printf("[SMOKE] ERROR: MM2S transfer failed\r\n");
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
 * @brief Run all three DMA smoke tests and print a summary.
 *
 * @details
 * Entry point called from main() before the full benchmark sweep.
 * Initialises the DMA driver once and runs the three tests in sequence.
 *
 * Expected outcomes:
 *   ACP read path : PASS — SCU snooped cache, data correct without flush
 *   HP0 read path : PASS — flush + invalidate discipline confirmed
 *   Stale data    : FAIL — omitting flush causes corruption (expected)
 *
 * If ACP or HP0 loopback fails, do not proceed to benchmark. Common causes:
 *   - DMA base address mismatch (verify XPAR_AXI_DMA_0_BASEADDR)
 *   - ARUSER/ARCACHE signals not set to 0xF on ACP protocol converter
 *   - AXI stream loopback missing (M_AXIS_MM2S not connected to S_AXIS_S2MM)
 *   - S2MM buffer length register too narrow (check Vivado DMA IP settings)
 *   - Buffer addresses outside PL-accessible DDR range (check address map)
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
