# figures4 — HW Matmul v2 (Tiled) Benchmark Results

**Board:** Digilent Zybo Z7-20 (XC7Z020-1CLG400C)  
**Logs:** `results_with_hw3.txt` + `results_with_sw` (UART capture, 115200 8N1)  
**Authors:** Brunda Marpadaga, Bhavana Marpadaga

This folder contains plots from the fourth benchmark run. HW v2 introduces cache-aware tiling — the kernel transposes B into B_T, then performs the N×N multiply in tiles so that the working set fits in the PS L1/L2 cache hierarchy during the compute phase.

---

## Files

| File | Description |
|------|-------------|
| `latency.png` | Elapsed time (µs) vs matrix side N for ACP, HP, SW, MATMUL |
| `l1_hit_rate.png` | L1 D-cache hit rate (%) vs N |
| `l2_hit_rate.png` | PL310 L2 cache hit rate (%) vs N |
| `speedup.png` | HP, SW, MATMUL latency normalised to ACP |
| `l1_misses.png` | Raw L1 D-cache miss count vs N |
| `hw_comparison.png` | HW v1 vs HW v2 vs SW latency and speedup bar chart |

---

## HW v1 vs HW v2 — Tiling Speedup

HW v1 had no tiling and accessed B column-by-column, thrashing the cache for large N. HW v2 transposes B into B_T first and reads both A and B_T row-by-row in tiles, keeping data in L2 across the inner loop.

| N | HW v1 (µs) | HW v2 (µs) | Speedup |
|---|-----------|-----------|---------|
| 32 | 22,672 | 1,985 | **11.4×** |
| 64 | 180,876 | 14,776 | **12.2×** |
| 128 | 1,445,154 | 114,070 | **12.7×** |
| 256 | 11,557,458 | 896,958 | **12.9×** |
| 512 | 101,325,293 | 7,113,180 | **14.2×** |

---

## HW v2 vs SW Matmul

| N | SW (µs) | HW v2 (µs) | Speedup |
|---|---------|-----------|---------|
| 32 | 2,421 | 1,985 | **1.22×** |
| 64 | 19,121 | 14,776 | **1.29×** |
| 128 | 152,666 | 114,070 | **1.34×** |
| 256 | 1,218,443 | 896,958 | **1.36×** |
| 512 | 9,740,454 | 7,113,180 | **1.37×** |

---

## Cache Behaviour (HW v2)

- **L1 hit rate: 100%** — tiled access keeps the working set in L1 during inner loops
- **L2 hit rate: ~50%** — B_T rows reused across the tile, hitting L2 on repeated access

---

## Comparison Context

| Folder | Contents |
|--------|----------|
| `figures - no accelerator/` | DMA baseline (ACP + HP0 only) |
| `figures2 - sw accelerator/` | Adds SW matmul |
| `figures3 - hw accelerator 1/` | HW v1 — untiled (~12× slower than v2) |
| `figures4-hwAccelerator2-tiling/` | **This folder** — HW v2, TILE=FETCH_TILE=4 |
| `figures5-hwAccelerator3-tiling-v2/` | HW v3 — FETCH_TILE=8 (~1.7× faster than v2) |
