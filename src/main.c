/**
 * main.c — ACP vs HP PS↔PL Coherency Benchmark
 * Member B: PS Software Engineer
 *
 * Build target: Vitis bare-metal application (Cortex-A9, Zynq-7000)
 * Toolchain:    arm-none-eabi-gcc (from Vitis unified IDE or classic SDK)
 *
 * Workflow
 * ─────────
 * 1. Member A exports .xsa from Vivado (contains AXI DMA or custom IP,
 *    wired to both ACP and HP ports, plus AXI_GP control registers).
 * 2. Member B: File → New → Platform Project → import .xsa → bare-metal.
 * 3. Create an Application Project on top of that platform, add main.c + pmu.h.
 * 4. Update the PL_IP_BASE address (search for ★ below) to match the actual
 *    XPAR_* symbol that Vitis generates from Member A's .xsa.
 *
 * What the benchmark measures
 * ────────────────────────────
 * For each matrix side N ∈ {32, 64, 128, 256, 512}:
 *
 *   ACP path (coherent):
 *     PS fills A[N×N] and B[N×N] in DDR (lines stay dirty in L1/L2).
 *     PS signals PL → PL reads A,B via ACP → SCU snoops PS caches →
 *     PL writes C via ACP → SCU updates PS cache.
 *     No explicit flush/invalidate needed.
 *
 *   HP path (non-coherent):
 *     PS fills A, B.
 *     PS calls Xil_DCacheFlushRange(A), Xil_DCacheFlushRange(B).
 *     PS signals PL → PL reads A,B via HP (straight to DDR controller).
 *     PL writes C via HP.
 *     PS calls Xil_DCacheInvalidateRange(C) before reading result.
 *     The flush+invalidate overhead is included in the measured time.
 *
 * Output: CSV rows on UART (115200 8N1), captured and fed to plot_results.py.
 *
 * CSV columns
 * ────────────
 * mode, N, elapsed_us, l1d_miss, l1d_access, l2d_miss, l2d_access,
 * l1_hit_pct (xx.yy), l2_hit_pct (xx.yy)
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "xil_printf.h"    /* xil_printf — lightweight, UART-backed          */
#include "xil_cache.h"     /* Xil_DCacheFlushRange / InvalidateRange          */
#include "xil_io.h"        /* Xil_Out32 / Xil_In32 for register MMIO         */
#include "xtime_l.h"       /* XTime_GetTime(), COUNTS_PER_SECOND              */
#include "xparameters.h"   /* Board-specific base addresses from .xsa         */
#include "pmu.h"           /* ARM Cortex-A9 PMU helpers                       */

/* ── Memory barriers (inline asm; no external dependency) ────────────────── */
#define DSB()  __asm__ volatile("dsb" ::: "memory")
#define DMB()  __asm__ volatile("dmb" ::: "memory")
#define ISB()  __asm__ volatile("isb" ::: "memory")

/* ─────────────────────────────────────────────────────────────────────────────
 * ★  PL HARDWARE INTERFACE — UPDATE AFTER IMPORTING MEMBER A's .xsa
 *
 * Expected custom AXI-Lite IP register map (4-byte registers, AXI_GP0):
 *
 *   Offset 0x00  CTRL_REG
 *                  [0]  start  — write 1 to begin transfer; self-clearing
 *                  [1]  mode   — 0 = ACP path, 1 = HP path
 *   Offset 0x04  STAT_REG
 *                  [0]  done   — PL sets 1 when DMA/compute finishes;
 *                               cleared automatically on next START
 *   Offset 0x08  SIZE_REG   — matrix side length N (uint32)
 *   Offset 0x0C  ADDR_A_REG — DDR base address of matrix A (uint32)
 *   Offset 0x10  ADDR_B_REG — DDR base address of matrix B (uint32)
 *   Offset 0x14  ADDR_C_REG — DDR base address of result   (uint32)
 *
 * If Member A uses AXI DMA + Interrupt instead of a custom IP, replace the
 * CTRL/STAT register writes below with XAxiDma_SimpleTransfer() calls and
 * poll XAxiDma_Busy() for completion.
 * ───────────────────────────────────────────────────────────────────────────*/
#define PL_IP_BASE    0x40400000   /* ★ replace with actual symbol */

#define PL_CTRL_ADDR  (PL_IP_BASE + 0x00U)
#define PL_STAT_ADDR  (PL_IP_BASE + 0x04U)
#define PL_SIZE_ADDR  (PL_IP_BASE + 0x08U)
#define PL_ADDRA_ADDR (PL_IP_BASE + 0x0CU)
#define PL_ADDRB_ADDR (PL_IP_BASE + 0x10U)
#define PL_ADDRC_ADDR (PL_IP_BASE + 0x14U)

#define CTRL_START    (1U << 0)
#define CTRL_MODE_HP  (1U << 1)   /* 0 = ACP, 1 = HP */
#define STAT_DONE     (1U << 0)

/* ─────────────────────────────────────────────────────────────────────────────
 * DDR MEMORY MAP
 *
 * Three non-overlapping regions in PS DDR (Zynq-7000, 512 MiB device).
 * 512×512 float matrix = 1 MiB; 4 MiB slots give plenty of headroom.
 * Start at 256 MiB to stay clear of Linux/U-Boot if used later.
 *
 * These addresses must also be programmed into the PL DMA via the registers
 * above; they are fixed so both PS and PL always agree.
 *
 * IMPORTANT: add a memory region for these addresses in the Vitis linker
 * script (lscript.ld) with no_cache attribute, OR keep the default cacheable
 * mapping and rely on the flush/invalidate calls (which is what we do here).
 * ───────────────────────────────────────────────────────────────────────────*/
#define MAT_A_BASE   0x10000000UL    /* matrix A — 256 MiB              */
#define MAT_B_BASE   0x10400000UL    /* matrix B — 256 MiB + 4 MiB      */
#define MAT_C_BASE   0x10800000UL    /* result C — 256 MiB + 8 MiB      */

#define MAT_A  ((volatile float *)MAT_A_BASE)
#define MAT_B  ((volatile float *)MAT_B_BASE)
#define MAT_C  ((volatile float *)MAT_C_BASE)

/* ─────────────────────────────────────────────────────────────────────────────
 * BENCHMARK PARAMETERS
 * ───────────────────────────────────────────────────────────────────────────*/
static const uint32_t SIZES[] = { 32, 64, 128, 256, 512 };
#define NUM_SIZES   (sizeof(SIZES) / sizeof(SIZES[0]))

/* Number of back-to-back runs per (mode, size); median is printed. */
#define REPEATS     5U

/* PL poll timeout — iterations before declaring a hang (~3–4 s at 666 MHz) */
#define POLL_TIMEOUT  0x20000000U

/* Cache-line size (bytes) — Cortex-A9 has 32-byte L1, 32-byte L2 lines;
   allocate and flush at 64-byte granularity to be safe across boards.       */
#define CACHELINE  64U

/* ─────────────────────────────────────────────────────────────────────────────
 * HELPER FUNCTIONS
 * ───────────────────────────────────────────────────────────────────────────*/

/** Round up byte count to next CACHELINE multiple (for aligned flush). */
static inline uint32_t align_up(uint32_t n)
{
    return (n + CACHELINE - 1U) & ~(CACHELINE - 1U);
}

/** Convert Global Timer ticks → microseconds.
 *  The Global Timer runs at CPU_CLK / 2 (COUNTS_PER_SECOND from xtime_l.h). */
static inline uint64_t ticks_to_us(XTime ticks)
{
    return (uint64_t)ticks * 1000000ULL / (uint64_t)COUNTS_PER_SECOND;
}

/** Fill an N×N float matrix with a deterministic pattern.
 *  Using volatile pointer to ensure the compiler actually stores to DDR. */
static void mat_fill(volatile float *m, uint32_t N, float seed)
{
    for (uint32_t i = 0; i < N * N; ++i)
        m[i] = seed + (float)i * 0.001f;
}

/** Write PL control/address registers, then assert START for one cycle. */
static inline void pl_configure_and_start(uint32_t N, uint32_t mode_bit)
{
    Xil_Out32(PL_SIZE_ADDR,  N);
    Xil_Out32(PL_ADDRA_ADDR, (uint32_t)MAT_A_BASE);
    Xil_Out32(PL_ADDRB_ADDR, (uint32_t)MAT_B_BASE);
    Xil_Out32(PL_ADDRC_ADDR, (uint32_t)MAT_C_BASE);
    DSB();   /* ensure register writes reach PL before the start pulse */
    Xil_Out32(PL_CTRL_ADDR, CTRL_START | mode_bit);
    DSB();
}

/** Poll STAT_DONE.  Returns 0 on success, -1 on timeout. */
static int pl_wait_done(void)
{
    for (uint32_t i = 0U; i < POLL_TIMEOUT; ++i) {
        if (Xil_In32(PL_STAT_ADDR) & STAT_DONE)
            return 0;
    }
    return -1;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * BENCHMARK PATHS
 * ───────────────────────────────────────────────────────────────────────────*/

/**
 * benchmark_coherent() — ACP path, no cache management.
 *
 * The PS has just written A and B (lines are dirty in L1/L2).
 * The PL reads them through the ACP; the Snoop Control Unit satisfies reads
 * from the PS cache, not stale DDR.  After the PL writes C via ACP, the SCU
 * invalidates the corresponding PS cache lines — the PS can read C directly.
 *
 * Measured interval: [PL start signal → PL done flag].
 *
 * Returns elapsed microseconds, or UINT64_MAX on timeout.
 */
static uint64_t benchmark_coherent(uint32_t N,
                                   pmu_counts_t *ps, pmu_counts_t *pe)
{
    XTime t0, t1;

    /* DMB: all preceding stores are globally visible before the start pulse */
    DMB();

    pmu_reset_counters();
    pmu_read_all(ps);
    XTime_GetTime(&t0);

    pl_configure_and_start(N, 0U);   /* mode_bit = 0 → ACP */

    if (pl_wait_done() != 0) {
        XTime_GetTime(&t1);
        pmu_read_all(pe);
        xil_printf("# WARNING: ACP timeout at N=%u\r\n", (unsigned)N);
        return UINT64_MAX;
    }

    /* No Xil_DCacheInvalidateRange needed — SCU handled coherency */

    XTime_GetTime(&t1);
    pmu_read_all(pe);

    return ticks_to_us(t1 - t0);
}

/**
 * benchmark_noncoherent() — HP path, explicit cache management.
 *
 * HP bypasses the SCU, so:
 *  • Before PL reads: flush A,B out of PS cache to DDR
 *    (Xil_DCacheFlushRange) — otherwise PL sees stale DDR.
 *  • After PL writes C to DDR via HP: invalidate C in PS cache
 *    (Xil_DCacheInvalidateRange) — otherwise PS reads its own stale copy.
 *
 * Both flush and invalidate costs are included in the measured interval so
 * the charts show real total latency for the non-coherent approach.
 *
 * Returns elapsed microseconds, or UINT64_MAX on timeout.
 */
static uint64_t benchmark_noncoherent(uint32_t N,
                                      pmu_counts_t *ps, pmu_counts_t *pe)
{
    XTime t0, t1;
    uint32_t bytes = align_up(N * N * (uint32_t)sizeof(float));

    pmu_reset_counters();
    pmu_read_all(ps);
    XTime_GetTime(&t0);

    /* ── 1. Flush: push dirty PS cache lines to DDR so HP port sees them ── */
    Xil_DCacheFlushRange((UINTPTR)MAT_A_BASE, bytes);
    Xil_DCacheFlushRange((UINTPTR)MAT_B_BASE, bytes);

    /* ── 2. Signal PL in HP mode ── */
    pl_configure_and_start(N, CTRL_MODE_HP);

    if (pl_wait_done() != 0) {
        XTime_GetTime(&t1);
        pmu_read_all(pe);
        xil_printf("# WARNING: HP timeout at N=%u\r\n", (unsigned)N);
        return UINT64_MAX;
    }

    /* ── 3. Invalidate C: discard any PS-side cache copy so we read DDR ── */
    Xil_DCacheInvalidateRange((UINTPTR)MAT_C_BASE, bytes);

    XTime_GetTime(&t1);
    pmu_read_all(pe);

    return ticks_to_us(t1 - t0);
}

/* ─────────────────────────────────────────────────────────────────────────────
 * MAIN — benchmark sweep + CSV output
 * ───────────────────────────────────────────────────────────────────────────*/
int main(void)
{
	xil_printf("Hello from Zybo Z7-20\r\n");

	pmu_init();
	l2_init();
	xil_printf("PMU and PL310 L2 counters initialised\r\n");

	/* Snapshot before */
	pmu_counts_t before, after;
	l2_counts_t l2b, l2a;

	uint32_t l2_id = *(volatile uint32_t *)(0xF8F02000UL + 0x000);
	uint32_t l2_ctrl = *(volatile uint32_t *)(0xF8F02000UL + 0x100);
	xil_printf("PL310 Cache ID   : 0x%08lX\r\n", (unsigned long)l2_id);
	xil_printf("PL310 Ctrl (en?) : 0x%08lX\r\n", (unsigned long)l2_ctrl);

	l2_reset();
	pmu_read_all(&before);
	l2_read(&l2b);

	/* Do something that definitely causes cache activity */
	volatile float buf[1024];
	for (int i = 0; i < 1024; i++) buf[i] = (float)i;
	for (int i = 0; i < 1024; i++) buf[i] *= 2.0f;


	/* Snapshot after */
	pmu_read_all(&after);
	l2_read(&l2a);

	xil_printf("L1 access delta   : %lu\r\n",
	               (unsigned long)(after.l1d_access - before.l1d_access));
	xil_printf("L1 miss delta     : %lu\r\n",
	               (unsigned long)(after.l1d_miss - before.l1d_miss));
	xil_printf("L2 access (DRREQ) : %lu\r\n",
	               (unsigned long)(l2a.drreq - l2b.drreq));
	xil_printf("L2 hit (DRHIT)    : %lu\r\n",
	               (unsigned long)(l2a.drhit - l2b.drhit));

	    while(1);
	    return 0;
}
