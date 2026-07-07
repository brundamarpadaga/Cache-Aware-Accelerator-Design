# figures5 — HW Matmul v3 (FETCH_TILE=8) Benchmark Results

**Board:** Digilent Zybo Z7-20 (XC7Z020-1CLG400C)  
**Logs:** `results_with_hw4.txt` + `results_with_sw` (UART capture, 115200 8N1)  
**Authors:** Brunda Marpadaga, Bhavana Marpadaga

This folder contains plots from the fifth benchmark run. HW v3 introduces a two-level tiling scheme in the `matmul_0` HLS kernel:

| Constant | Value | Role |
|----------|-------|------|
| `FETCH_TILE` | 8 | DDR fetch tile — 8×8 = 64 floats fetched per AXI round-trip |
| `TILE` | 4 | Compute sub-tile — 4×4 unrolled MAC engine (DSP count unchanged) |
| `T_TILE` | 16 | Transpose tile — 16×16 block for the B→B_T transpose step |

> **N must be a multiple of 16** (max of `FETCH_TILE` and `T_TILE`). Benchmark sweep sizes N=32,64,128,256,512 all satisfy this. The correctness check was updated from N=4 (unsupported) to N=32.

---

## Files

| File | Description |
|------|-------------|
| `latency.png` | Elapsed time (µs) vs matrix side N for ACP, HP, SW, MATMUL |
| `l1_hit_rate.png` | L1 D-cache hit rate (%) vs N |
| `l2_hit_rate.png` | PL310 L2 cache hit rate (%) vs N |
| `speedup.png` | HP, SW, MATMUL latency normalised to ACP |
| `l1_misses.png` | Raw L1 D-cache miss count vs N |
| `hw_comparison.png` | HW v2 vs HW v3 vs SW latency and speedup bar chart |

---

## Median Latency Results

| N | ACP (µs) | HP (µs) | SW (µs) | HW v3 (µs) |
|---|----------|---------|---------|------------|
| 32 | 32 | 58 | 2,421 | 1,194 |
| 64 | 112 | 211 | 19,121 | 8,545 |
| 128 | 429 | 828 | 152,666 | 64,588 |
| 256 | 1,697 | 3,236 | 1,218,443 | 502,071 |
| 512 | 6,772 | 10,655 | 9,740,454 | 3,962,935 |

---

## HW v3 vs HW v2 (FETCH_TILE=4 → FETCH_TILE=8)

Larger fetch tile reduces DDR round-trips — the same TILE=4 compute engine now processes a bigger on-chip block before going back to DDR, improving bandwidth utilisation without increasing DSP count.

| N | HW v2 (µs) | HW v3 (µs) | Speedup |
|---|-----------|-----------|---------|
| 32 | 1,985 | 1,194 | **1.66×** |
| 64 | 14,776 | 8,545 | **1.73×** |
| 128 | 114,070 | 64,588 | **1.77×** |
| 256 | 896,958 | 502,071 | **1.79×** |
| 512 | 7,113,180 | 3,962,935 | **1.79×** |

---

## HW v3 vs SW Matmul

| N | SW (µs) | HW v3 (µs) | Speedup |
|---|---------|-----------|---------|
| 32 | 2,421 | 1,194 | **2.03×** |
| 64 | 19,121 | 8,545 | **2.24×** |
| 128 | 152,666 | 64,588 | **2.36×** |
| 256 | 1,218,443 | 502,071 | **2.43×** |
| 512 | 9,740,454 | 3,962,935 | **2.46×** |

---

## Cache Behaviour (HW v3)

- **L1 hit rate: 100%** — tiled access keeps the working set in L1 during inner loops
- **L2 hit rate: ~49–50%** — B_T rows reused across the tile, hitting L2 on repeated access

---

## Comparison Context

| Folder | Contents |
|--------|----------|
| `figures - no accelerator/` | DMA baseline (ACP + HP0 only) |
| `figures2 - sw accelerator/` | Adds SW matmul |
| `figures3 - hw accelerator 1/` | HW v1 — untiled (~12× slower than v2) |
| `figures4-hwAccelerator2-tiling/` | HW v2 — TILE=FETCH_TILE=4 |
| `figures5-hwAccelerator3-tiling-v2/` | **This folder** — HW v3, FETCH_TILE=8 |
