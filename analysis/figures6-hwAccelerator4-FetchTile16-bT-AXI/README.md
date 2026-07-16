# figures6 — HW Matmul v4 (FETCH_TILE=16, Dedicated B_T AXI) Benchmark Results

**Board:** Digilent Zybo Z7-20 (XC7Z020-1CLG400C)  
**Logs:** `results_with_hw5_bT_AXI.txt` + `results_with_sw` (UART capture, 115200 8N1)  
**Authors:** Brunda Marpadaga, Bhavana Marpadaga

This folder contains plots from the sixth benchmark run. HW v4 introduces two changes over HW v3:

| Change | HW v3 | HW v4 | Effect |
|--------|-------|-------|--------|
| `FETCH_TILE` | 8 | 16 | 4× more data fetched per DDR round-trip (16×16 = 256 floats) |
| B_T AXI bundle | Shared with A, B on `ACP` | Dedicated `BT` bundle (`num_write_outstanding=1`) | Eliminates AXI write/read race between transpose and matmul stages |

> **N must be a multiple of 16** (`FETCH_TILE=16`, `T_TILE=16`, `TILE=4`). All benchmark sweep sizes (32, 64, 128, 256, 512) satisfy this. Correctness check runs at N=16 — the minimum valid size and a boundary test (exactly one tile).

---

## Files

| File | Description |
|------|-------------|
| `latency.png` | Elapsed time (µs) vs matrix side N for ACP, HP, SW, MATMUL |
| `l1_hit_rate.png` | L1 D-cache hit rate (%) vs N |
| `l2_hit_rate.png` | PL310 L2 cache hit rate (%) vs N |
| `speedup.png` | HP, SW, MATMUL latency normalised to ACP |
| `l1_misses.png` | Raw L1 D-cache miss count vs N |
| `hw_comparison.png` | HW v3 vs HW v4 vs SW latency and speedup bar chart |

---

## Median Latency Results

| N | ACP (µs) | HP (µs) | SW (µs) | HW v4 (µs) |
|---|----------|---------|---------|------------|
| 32 | 32 | 57 | 2,421 | 686 |
| 64 | 112 | 211 | 19,121 | 4,612 |
| 128 | 429 | 828 | 152,666 | 33,552 |
| 256 | 1,698 | 3,232 | 1,218,443 | 255,167 |
| 512 | 6,772 | 10,655 | 9,740,454 | 1,993,172 |

---

## HW v4 vs HW v3

Both changes together deliver close to **2× speedup** over HW v3 at every matrix size. The FETCH_TILE increase reduces DDR round-trips from N/8 to N/16 per output tile; the dedicated B_T AXI bundle removes bus contention between transpose writes and matmul reads on the shared ACP port.

| N | HW v3 (µs) | HW v4 (µs) | Speedup |
|---|-----------|-----------|---------|
| 32 | 1,194 | 686 | **1.74×** |
| 64 | 8,545 | 4,612 | **1.85×** |
| 128 | 64,588 | 33,552 | **1.93×** |
| 256 | 502,071 | 255,167 | **1.97×** |
| 512 | 3,962,935 | 1,993,172 | **1.99×** |

The speedup grows with N (1.74× at N=32 → ~2× at N=512), which is expected — fixed overhead (tile setup, AXI handshaking) amortises over more compute at larger matrix sizes.

---

## HW v4 vs SW Matmul

HW v4 now beats ARM Cortex-A9 software matmul by **3.5–4.9×**, up from 2.0–2.5× in HW v3.

| N | SW (µs) | HW v4 (µs) | Speedup |
|---|---------|-----------|---------|
| 32 | 2,421 | 686 | **3.53×** |
| 64 | 19,121 | 4,612 | **4.15×** |
| 128 | 152,666 | 33,552 | **4.55×** |
| 256 | 1,218,443 | 255,167 | **4.78×** |
| 512 | 9,740,454 | 1,993,172 | **4.89×** |

The increasing speedup with N reflects the tiling benefit compounding — larger matrices spend a greater fraction of time in the compute phase (which is fully parallelised) rather than in fixed overhead.

---

## Key Observations

### FETCH_TILE=16 impact
Doubling FETCH_TILE from 8 to 16 cuts DDR round-trips per output tile by 2×. Since the kernel is memory-bandwidth-bound (load time dominates compute time), fewer, larger bursts translate almost linearly into wall-clock speedup. The ~2× improvement seen here is consistent with the ~1.79× from FETCH_TILE=4→8 in the previous iteration, and confirms the diminishing-returns trend — the next doubling to FETCH_TILE=32 would hit BRAM limits before fitting on device.

### Dedicated B_T AXI bundle
With `num_write_outstanding=1` on its own bundle, the AXI adapter now drains each transpose write (waits for BRESP) before issuing the next command. This eliminates the write/read race diagnosed in the matmul README (Section 9) where `transpose`'s writes to B_T could still be in-flight when `matmul_tiled` started reading them back. The combined effect of this fix and the FETCH_TILE increase makes the ~2× improvement difficult to attribute to either change alone, but both are necessary — the race fix ensures correctness at all N, and FETCH_TILE=16 provides the performance gain.

### Cache behaviour
- **L1 hit rate: 100%** — tiled access keeps the working set in L1 during inner loops
- **L2 hit rate: ~42–49%** — slightly lower than HW v3's ~49–50%, likely because FETCH_TILE=16 tiles are larger and evict more L2 lines per round-trip, but the DDR bandwidth saving more than compensates

### Progression summary

| Version | Key change | N=512 latency | vs SW |
|---------|-----------|---------------|-------|
| HW v1 | No tiling, strided B access | 101,325,293 µs | 0.10× (slower) |
| HW v2 | Tiling (TILE=FETCH_TILE=4) + B_T transpose | 7,113,180 µs | 1.37× |
| HW v3 | FETCH_TILE=8 (larger DDR fetch tile) | 3,962,935 µs | 2.46× |
| HW v4 | FETCH_TILE=16 + dedicated B_T AXI | 1,993,172 µs | **4.89×** |

---

## Comparison Context

| Folder | Contents |
|--------|----------|
| `figures - no accelerator/` | DMA baseline (ACP + HP0 only) |
| `figures2 - sw accelerator/` | Adds SW matmul |
| `figures3 - hw accelerator 1/` | HW v1 — untiled |
| `figures4-hwAccelerator2-tiling/` | HW v2 — TILE=FETCH_TILE=4 |
| `figures5-hwAccelerator3-tiling-v2/` | HW v3 — FETCH_TILE=8 |
| `figures6-hwAccelerator4-FetchTile16-bT-AXI/` | **This folder** — HW v4, FETCH_TILE=16, B_T dedicated AXI |
