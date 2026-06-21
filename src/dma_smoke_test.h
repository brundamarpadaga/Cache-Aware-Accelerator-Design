/**
 * @file   dma_smoke_test.h
 * @author Brunda Marpadaga (brundamarpadaga@gmail.com)
 * @author Bhavana Marpadaga (marapadagabhavana@gmail.com)
 * @brief  Shared declarations for the AXI DMA loopback smoke tests.
 *
 * @note Portions of this code were generated with assistance from Claude AI (Anthropic).
 */

#ifndef DMA_SMOKE_TEST_H
#define DMA_SMOKE_TEST_H

#include "xaxidma.h"   /* XAxiDma type needed for extern declaration */

/* ── Buffer addresses (must match main.c DDR memory map) ─────────────────── */

/** @brief Source buffer base address — matrix A region in DDR (256MB mark). */
#define SMOKE_SRC_BASE   0x10000000UL

/** @brief Destination buffer base address — matrix C region in DDR. */
#define SMOKE_DST_BASE   0x10800000UL

/** @brief Number of floats per smoke test transfer (256×256 = 64KB). */
#define SMOKE_N          256U

/** @brief Total bytes per transfer. */
#define SMOKE_BYTES      (SMOKE_N * SMOKE_N * sizeof(float))

/* ── Shared DMA instance ─────────────────────────────────────────────────────
 * Defined (non-static) in dma_smoke_test.c, initialised by run_dma_smoke_tests().
 * benchmark.c accesses it via this extern — do NOT call CfgInitialize again. */
extern XAxiDma dma;

/* ── Shared helper ───────────────────────────────────────────────────────────
 * Polls MM2S then S2MM status registers until both idle, issues a DSB.
 * Returns 0 on success, -1 on timeout.                                       */
int dma_wait_done(void);


void run_dma_smoke_tests(void);


#endif
