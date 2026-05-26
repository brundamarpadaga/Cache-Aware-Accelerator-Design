/**
 * pmu.h — ARM Cortex-A9 Performance Monitor Unit helpers
 *
 * Configures four hardware event counters:
 *   CTR 0 → L1D cache miss  (refill)      event 0x03
 *   CTR 1 → L1D cache access              event 0x04
 *   CTR 2 → L2D cache miss  (impl-def)    event 0x16
 *   CTR 3 → L2D cache access (impl-def)   event 0x14
 *
 * NOTE on L2 events (0x14, 0x16):
 *   These are implementation-defined for Cortex-A9.  Zynq-7000 routes them
 *   through the PL310 L2 controller; they work on silicon but may return zero
 *   on some emulators.  If they read zero consistently, read the PL310 event
 *   registers directly at L2CC_BASE + 0x200/0x204 instead.
 *
 * All CP15 accesses require privileged mode — always true in bare-metal.
 */

#ifndef PMU_H
#define PMU_H

#include <stdint.h>

/* ── Event IDs ──────────────────────────────────────────────────────────── */
#define PMU_EVT_L1D_MISS     0x03U   /* L1 data cache refill (miss→linefill)  */
#define PMU_EVT_L1D_ACCESS   0x04U   /* L1 data cache access                  */
#define PMU_EVT_L2D_ACCESS   0x14U   /* L2D access  (implementation-defined)  */
#define PMU_EVT_L2D_MISS     0x16U   /* L2D miss    (implementation-defined)  */

/* ── Counter slot assignments ───────────────────────────────────────────── */
#define PMU_CTR_L1D_MISS     0U
#define PMU_CTR_L1D_ACCESS   1U
#define PMU_CTR_L2D_MISS     2U
#define PMU_CTR_L2D_ACCESS   3U

/* ── PMCR bit masks ─────────────────────────────────────────────────────── */
#define PMCR_E  (1U << 0)   /* Enable all counters          */
#define PMCR_P  (1U << 1)   /* Reset all event counters     */
#define PMCR_C  (1U << 2)   /* Reset cycle counter          */

/* Snapshot of all four event counters */
typedef struct {
    uint32_t l1d_miss;
    uint32_t l1d_access;
    uint32_t l2d_miss;
    uint32_t l2d_access;
} pmu_counts_t;

/* ── CP15 register accessors ────────────────────────────────────────────── */

static inline uint32_t _pmu_rd_pmcr(void)
{
    uint32_t v;
    __asm__ volatile("MRC p15, 0, %0, c9, c12, 0" : "=r"(v));
    return v;
}
static inline void _pmu_wr_pmcr(uint32_t v)
{
    __asm__ volatile("MCR p15, 0, %0, c9, c12, 0" :: "r"(v) : "memory");
}
/* PMCNTENSET — enable individual counters */
static inline void _pmu_wr_cntenset(uint32_t v)
{
    __asm__ volatile("MCR p15, 0, %0, c9, c12, 1" :: "r"(v) : "memory");
}
/* PMSELR — select which event counter to address */
static inline void _pmu_sel(uint32_t n)
{
    __asm__ volatile("MCR p15, 0, %0, c9, c12, 5" :: "r"(n) : "memory");
}
/* PMXEVTYPER — set event type for selected counter */
static inline void _pmu_set_event(uint32_t ev)
{
    __asm__ volatile("MCR p15, 0, %0, c9, c13, 1" :: "r"(ev) : "memory");
}
/* PMXEVCNTR — read event count for selected counter */
static inline uint32_t _pmu_rd_cnt(void)
{
    uint32_t v;
    __asm__ volatile("MRC p15, 0, %0, c9, c13, 2" : "=r"(v));
    return v;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

/**
 * pmu_init() — call once at startup.
 * Resets all counters, configures the four event types, and enables counting.
 */
static inline void pmu_init(void)
{
    /* Reset everything first */
    _pmu_wr_pmcr(PMCR_P | PMCR_C);

    /* Wire up the four event counters */
    _pmu_sel(PMU_CTR_L1D_MISS);   _pmu_set_event(PMU_EVT_L1D_MISS);
    _pmu_sel(PMU_CTR_L1D_ACCESS);  _pmu_set_event(PMU_EVT_L1D_ACCESS);
    _pmu_sel(PMU_CTR_L2D_MISS);    _pmu_set_event(PMU_EVT_L2D_MISS);
    _pmu_sel(PMU_CTR_L2D_ACCESS);  _pmu_set_event(PMU_EVT_L2D_ACCESS);

    /* Enable counters 0-3 (bits [3:0]) and cycle counter (bit 31) */
    _pmu_wr_cntenset(0x8000000FU);

    /* Start counting */
    _pmu_wr_pmcr(_pmu_rd_pmcr() | PMCR_E);
}

/**
 * pmu_reset_counters() — zero all event counters and cycle counter.
 * Call immediately before each timed section.
 */
static inline void pmu_reset_counters(void)
{
    _pmu_wr_pmcr(_pmu_rd_pmcr() | PMCR_P | PMCR_C);
}

/**
 * pmu_read_all() — snapshot all four counters into *c.
 * Call once before and once after the timed region; subtract to get deltas.
 */
static inline void pmu_read_all(pmu_counts_t *c)
{
    _pmu_sel(PMU_CTR_L1D_MISS);   c->l1d_miss   = _pmu_rd_cnt();
    _pmu_sel(PMU_CTR_L1D_ACCESS);  c->l1d_access = _pmu_rd_cnt();
    _pmu_sel(PMU_CTR_L2D_MISS);    c->l2d_miss   = _pmu_rd_cnt();
    _pmu_sel(PMU_CTR_L2D_ACCESS);  c->l2d_access = _pmu_rd_cnt();
}

#endif /* PMU_H */
