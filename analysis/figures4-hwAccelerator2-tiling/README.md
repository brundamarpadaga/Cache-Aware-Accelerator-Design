figures4-hwAccelerator2-tiling/ — Benchmark Results: ACP + HP0 + SW + HW Matmul v2 (Tiled)
==============================================================================================
Board:   Digilent Zybo Z7-20 (XC7Z020-1CLG400C)
Log:     results_with_hw3.txt + results_with_sw (UART capture, 115200 8N1)
Authors: Brunda Marpadaga, Bhavana Marpadaga

This folder contains plots from the fourth benchmark run. The key addition is Bhavana's
revised HLS matmul_0 accelerator (HW v2), which introduces cache-aware tiling — the
kernel transposes B into B_T, then performs the N×N multiply in tiles so that the working
set fits in the PS L1/L2 cache hierarchy during the compute phase.

Files
-----
latency.png      Time (µs) vs matrix side N for all four modes.
                   ACP    — DMA loopback via ACP (SCU snooping, no flush)
                   HP     — DMA loopback via HP0 (PS flush + invalidate)
                   SW     — Software N×N matmul on ARM Cortex-A9, cold-cache start
                   MATMUL — HW v2 tiled matmul accelerator (matmul_0, 50 MHz)

l1_hit_rate.png  L1 D-cache hit rate (%) vs N.
l2_hit_rate.png  PL310 L2 cache hit rate (%) vs N.
speedup.png      HP, SW, MATMUL elapsed time normalised to ACP elapsed time.
l1_misses.png    Raw L1 D-cache miss count vs N for each mode.

HW v1 vs HW v2 — Tiling Speedup
---------------------------------
The previous accelerator (HW v1, results_with_hw.log) did not use tiling and accessed
B column-by-column, thrashing the cache for large N. HW v2 transposes B into B_T first
and then reads both A and B_T row-by-row in tiles, keeping data in L2 across the inner loop.

  N     HW v1 (µs)   HW v2 (µs)   Speedup
  ---   ----------   ----------   -------
   32       22,672        1,985    11.4×
   64      180,876       14,776    12.2×
  128    1,445,154      114,070    12.7×
  256   11,557,458      896,958    12.9×
  512  101,325,293    7,113,180    14.2×

HW v2 vs SW Matmul
-------------------
The tiled HW accelerator now outperforms the ARM Cortex-A9 software matmul at every
matrix size (1.2–1.4× faster), demonstrating that the cache-aware design recovers the
hardware's bandwidth advantage over a pure-software implementation.

  N     SW (µs)   HW v2 (µs)   HW speedup over SW
  ---   -------   ----------   ------------------
   32     2,421        1,985    1.22×
   64    19,121       14,776    1.29×
  128   152,666      114,070    1.34×
  256 1,218,443      896,958    1.36×
  512 9,740,454    7,113,180    1.37×

Cache Behaviour (HW v2)
------------------------
L1 hit rate: 100% — tiled access keeps the working set in L1 during inner loops.
L2 hit rate: ~50% — B_T rows are reused across the tile, hitting L2 on repeated access.
This confirms the tiling strategy is effective: the accelerator is compute-bound rather
than memory-bandwidth-bound for the inner tile loop.

Comparison Context
------------------
analysis/figures/                      DMA-only baseline (ACP + HP0, no matmul)
analysis/figures2 - sw accelerator/    Adds SW matmul (ACP + HP0 + SW)
analysis/figures3 - hw accelerator 1/  HW v1 results (untiled, ~12× slower than v2)
analysis/figures4-hwAccelerator2-tiling/  This folder — HW v2 tiled results
