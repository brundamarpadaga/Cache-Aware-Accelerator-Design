#ifndef BENCHMARK_H
#define BENCHMARK_H

/**
 * run_benchmark_sweep() — ACP vs HP0 latency and cache counter sweep.
 *
 * Prerequisites (must be done in main() before calling this):
 *   1. pmu_init()            — ARM PMU CP15 counters
 *   2. l2_init()             — PL310 MMIO counters
 *   3. run_dma_smoke_tests() — initialises XAxiDma `dma`, confirms HW wiring
 *
 * Output: CSV rows on UART (115200 8N1).
 * Post-process with: python3 plot_results.py results.log --out figures/
 */
void run_benchmark_sweep(void);

#endif /* BENCHMARK_H */
