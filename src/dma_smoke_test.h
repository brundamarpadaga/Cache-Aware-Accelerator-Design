#ifndef DMA_SMOKE_TEST_H
#define DMA_SMOKE_TEST_H

/* ── Buffer addresses (must match main.c DDR memory map) ─────────────────── */

/** @brief Source buffer base address — matrix A region in DDR (256MB mark). */
#define SMOKE_SRC_BASE   0x10000000UL

/** @brief Destination buffer base address — matrix C region in DDR. */
#define SMOKE_DST_BASE   0x10800000UL

/** @brief Number of floats per smoke test transfer (256×256 = 64KB). */
#define SMOKE_N          256U

/** @brief Total bytes per transfer. */
#define SMOKE_BYTES      (SMOKE_N * SMOKE_N * sizeof(float))

void run_dma_smoke_tests(void);


#endif
