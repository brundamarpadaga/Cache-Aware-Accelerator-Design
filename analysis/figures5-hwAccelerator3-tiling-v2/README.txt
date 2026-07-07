figures5-hwAccelerator3-tiling-v2/ — Benchmark Results: ACP + HP0 + SW + HW Matmul v3 (FETCH_TILE=8)
=======================================================================================================
Board:   Digilent Zybo Z7-20 (XC7Z020-1CLG400C)
Log:     results_with_hw4.txt + results_with_sw (UART capture, 115200 8N1)
Authors: Brunda Marpadaga, Bhavana Marpadaga

This folder contains plots from the fifth benchmark run. HW v3 introduces a two-level
tiling scheme in the matmul_0 HLS kernel:

  FETCH_TILE = 8  — DDR fetch tile: 8x8 = 64 floats fetched per AXI round-trip
  TILE       = 4  — Compute sub-tile: 4x4 unrolled MAC engine (DSP count unchanged)
  T_TILE     = 16 — Transpose tile: 16x16 block for the B→B_T transpose step

N must be a multiple of 16 (max of FETCH_TILE and T_TILE). Benchmark sweep sizes
N=32,64,128,256,512 all satisfy this constraint. The correctness check was updated
from N=4 (unsupported — smaller than FETCH_TILE) to N=32.

Files
-----
latency.png      Time (us) vs matrix side N for all four modes.
                   ACP    — DMA loopback via ACP (coherent, no flush)
                   HP     — DMA loopback via HP0 (flush + invalidate)
                   SW     — Software N×N matmul on ARM Cortex-A9, cold-cache
                   MATMUL — HW v3 tiled accelerator (matmul_0, FETCH_TILE=8)

l1_hit_rate.png  L1 D-cache hit rate (%) vs N.
l2_hit_rate.png  PL310 L2 cache hit rate (%) vs N.
speedup.png      HP, SW, MATMUL elapsed time normalised to ACP elapsed time.
l1_misses.png    Raw L1 D-cache miss count vs N for each mode.
hw_comparison.png  HW v2 vs HW v3 vs SW latency and speedup bar chart.

Median Latency Results
----------------------
  N     ACP (us)   HP (us)   SW (us)    HW v3 (us)
  ---   --------   -------   --------   ----------
   32         32        58      2,421        1,590
   64        112       211     19,121       11,661
  128        429       828    152,666       89,329
  256      1,698     3,236  1,218,443      699,515
  512      6,773    10,656  9,740,454    5,538,054

HW v3 vs HW v2 (FETCH_TILE=4 → FETCH_TILE=8)
----------------------------------------------
Larger fetch tile reduces DDR round-trips — the same TILE=4 compute engine now
processes a bigger on-chip block before going back to DDR, improving bandwidth
utilisation without increasing DSP count.

  N     HW v2 (us)   HW v3 (us)   Speedup (v2/v3)
  ---   ----------   ----------   ---------------
   32        1,985        1,590           1.25x
   64       14,776       11,661           1.27x
  128      114,070       89,329           1.28x
  256      896,958      699,515           1.28x
  512    7,113,180    5,538,054           1.28x

HW v3 vs SW Matmul
-------------------
  N     SW (us)      HW v3 (us)   HW speedup over SW
  ---   ----------   ----------   ------------------
   32        2,421        1,590           1.52x
   64       19,121       11,661           1.64x
  128      152,666       89,329           1.71x
  256    1,218,443      699,515           1.74x
  512    9,740,454    5,538,054           1.76x

Cache Behaviour (HW v3)
------------------------
L1 hit rate: 100% — tiled access keeps working set in L1 during inner loops.
L2 hit rate: ~50% — B_T rows reused across the tile, hitting L2 on repeated access.

Comparison Context
------------------
analysis/figures - no accelerator/         DMA baseline (ACP + HP0)
analysis/figures2 - sw accelerator/        Adds SW matmul
analysis/figures3 - hw accelerator 1/      HW v1 (untiled, ~12x slower than v2)
analysis/figures4-hwAccelerator2-tiling/   HW v2 (TILE=FETCH_TILE=4)
analysis/figures5-hwAccelerator3-tiling-v2/  This folder — HW v3 (FETCH_TILE=8)
