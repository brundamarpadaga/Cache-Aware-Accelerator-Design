# figures2 — ACP + HP0 + SW Matmul Benchmark Results

**Board:** Digilent Zybo Z7-20 (XC7Z020-1CLG400C)  
**Log:** `results_with_sw` (UART capture, 115200 8N1)  
**Authors:** Brunda Marpadaga, Bhavana Marpadaga

This folder contains plots from the second benchmark run, which added a software matrix multiply (SW) mode alongside the existing ACP and HP0 DMA loopback benchmarks. The ARM CPU performs the full N×N multiply in software — no hardware accelerator yet.

---

## Files

| File | Description |
|------|-------------|
| `latency.png` | Elapsed time (µs) vs matrix side N for ACP, HP, SW |
| `l1_hit_rate.png` | L1 D-cache hit rate (%) vs N — ARM PMU events 0x03 / 0x04 |
| `l2_hit_rate.png` | PL310 L2 cache hit rate (%) vs N — MMIO counters at 0xF8F02000 |
| `speedup.png` | HP and SW latency normalised to ACP — ratio > 1 means ACP is faster |
| `l1_misses.png` | Raw L1 D-cache miss count vs N for each mode |

---

## Comparison Context

| Folder | Contents |
|--------|----------|
| `figures - no accelerator/` | DMA baseline (ACP + HP0 only) |
| `figures2 - sw accelerator/` | **This folder** — adds SW matmul |
| `figures3 - hw accelerator 1/` | HW v1 — untiled accelerator |
| `figures4-hwAccelerator2-tiling/` | HW v2 — tiled accelerator (FETCH_TILE=4) |
| `figures5-hwAccelerator3-tiling-v2/` | HW v3 — tiled accelerator (FETCH_TILE=8) |
