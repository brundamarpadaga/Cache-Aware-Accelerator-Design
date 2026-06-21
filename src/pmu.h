/**
 * @file   pmu.h
 * @author Brunda Marpadaga (brundamarpadaga@gmail.com)
 * @author Bhavana Marpadaga (marapadagabhavana@gmail.com)
 * @brief  ARM PMU CP15 and PL310 L2 cache counter helpers for Zynq-7000.
 *
 * @note Portions of this code were generated with assistance from Claude AI (Anthropic).
 */

#ifndef PMU_H
#define PMU_H

#include <stdint.h>

/* ── PL310 L2 Cache Controller ───────────────────────────────────────────
 * Standalone PL310 on Zynq-7000 — not wired into Cortex-A9 PMU.
 * Read via MMIO at L2CC base address (UG585 Table 4-1).
 *
 * CTR0 → DRREQ (data read requests = total L2 accesses)
 * CTR1 → DRHIT (data read hits)
 * L2 hit rate = DRHIT / DRREQ
 * ─────────────────────────────────────────────────────────────────────── */
#define L2CC_BASE       0xF8F02000UL
#define L2CC_ECNTR_CTRL (*(volatile uint32_t *)(L2CC_BASE + 0x200))
#define L2CC_ECFGR1     (*(volatile uint32_t *)(L2CC_BASE + 0x204))
#define L2CC_ECFGR0     (*(volatile uint32_t *)(L2CC_BASE + 0x208))
#define L2CC_ECNTR1     (*(volatile uint32_t *)(L2CC_BASE + 0x20C))
#define L2CC_ECNTR0     (*(volatile uint32_t *)(L2CC_BASE + 0x210))

/* Event source IDs — written to bits[7:2] of ECFGR */
#define PL310_EVT_DRHIT (0x2U << 2)   /* 0x08 — data read hit     */
#define PL310_EVT_DRREQ (0x3U << 2)   /* 0x0C — data read request */

typedef struct {
    uint32_t drreq;   /* total L2 data read accesses */
    uint32_t drhit;   /* L2 data read hits           */
} l2_counts_t;

static inline void l2_init(void)
{
    L2CC_ECNTR_CTRL = 0x7U;           /* reset CTR0, reset CTR1, enable */
    L2CC_ECFGR0     = PL310_EVT_DRREQ; /* CTR0 = data read requests      */
    L2CC_ECFGR1     = PL310_EVT_DRHIT; /* CTR1 = data read hits          */
}

static inline void l2_reset(void)
{
    L2CC_ECNTR_CTRL = 0x7U;           /* reset CTR0 + CTR1 + keep enabled */
}

static inline void l2_read(l2_counts_t *c)
{
    c->drreq = L2CC_ECNTR0;
    c->drhit = L2CC_ECNTR1;
}

/* ── ARM PMU — L1 cache counters via CP15 ───────────────────────────────── */
#define PMU_EVT_L1D_MISS    0x03U
#define PMU_EVT_L1D_ACCESS  0x04U

#define PMU_CTR_L1D_MISS    0U
#define PMU_CTR_L1D_ACCESS  1U

#define PMCR_E  (1U << 0)
#define PMCR_P  (1U << 1)
#define PMCR_C  (1U << 2)

typedef struct {
    uint32_t l1d_miss;
    uint32_t l1d_access;
} pmu_counts_t;

static inline uint32_t _pmu_rd_pmcr(void)
{ uint32_t v; __asm__ volatile("MRC p15, 0, %0, c9, c12, 0" : "=r"(v)); return v; }

static inline void _pmu_wr_pmcr(uint32_t v)
{ __asm__ volatile("MCR p15, 0, %0, c9, c12, 0" :: "r"(v) : "memory"); }

static inline void _pmu_wr_cntenset(uint32_t v)
{ __asm__ volatile("MCR p15, 0, %0, c9, c12, 1" :: "r"(v) : "memory"); }

static inline void _pmu_sel(uint32_t n)
{ __asm__ volatile("MCR p15, 0, %0, c9, c12, 5" :: "r"(n) : "memory"); }

static inline void _pmu_set_event(uint32_t ev)
{ __asm__ volatile("MCR p15, 0, %0, c9, c13, 1" :: "r"(ev) : "memory"); }

static inline uint32_t _pmu_rd_cnt(void)
{ uint32_t v; __asm__ volatile("MRC p15, 0, %0, c9, c13, 2" : "=r"(v)); return v; }

static inline void pmu_init(void)
{
    _pmu_wr_pmcr(PMCR_P | PMCR_C);
    _pmu_sel(PMU_CTR_L1D_MISS);   _pmu_set_event(PMU_EVT_L1D_MISS);
    _pmu_sel(PMU_CTR_L1D_ACCESS); _pmu_set_event(PMU_EVT_L1D_ACCESS);
    _pmu_wr_cntenset(0x80000003U); /* enable CTR0, CTR1, CCNT */
    _pmu_wr_pmcr(_pmu_rd_pmcr() | PMCR_E);
}

static inline void pmu_reset_counters(void)
{ _pmu_wr_pmcr(_pmu_rd_pmcr() | PMCR_P | PMCR_C); }

static inline void pmu_read_all(pmu_counts_t *c)
{
    _pmu_sel(PMU_CTR_L1D_MISS);   c->l1d_miss   = _pmu_rd_cnt();
    _pmu_sel(PMU_CTR_L1D_ACCESS); c->l1d_access = _pmu_rd_cnt();
}

#endif /* PMU_H */
