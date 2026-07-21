# Cache-Aware Accelerator Design: ACP vs HP0 and HLS Matrix Multiply on Zynq-7020

> Benchmarking coherent vs non-coherent PS↔PL memory paths and a cache-aware tiled matrix multiply HLS accelerator on the Xilinx Zynq-7020 SoC.

**Board:** Digilent Zybo Z7-20 (XC7Z020-1CLG400C, 1GB DDR3)  
**Toolchain:** Vivado 2022.2 · Vitis 2022.2 · arm-none-eabi-gcc  
**Authors:** Brunda Marpadaga · Bhavana Marpadaga

---

## Project Overview

This project has two parallel goals:

**1. Memory path comparison (ACP vs HP0)**  
The Zynq-7020 exposes two distinct pathways for PL↔PS memory transfers. ACP routes through the Snoop Control Unit and participates in ARM cache coherency — no explicit cache management needed. HP0 bypasses the SCU and goes straight to DDR — higher raw bandwidth but requires explicit flush/invalidate or silent data corruption occurs. We benchmark both paths across matrix sizes N=32–512 using AXI DMA loopback.

**2. HLS matrix multiply accelerator (SW vs HW)**  
A tiled matrix multiply kernel (`matmul_0`) is implemented in Vitis HLS and benchmarked against a pure ARM software baseline. The accelerator uses two-level tiling (FETCH_TILE=16 for DDR burst efficiency, TILE=4 for the unrolled MAC engine), transposes B into a scratch buffer for sequential access, and routes reads via ACP and writes via HP0.

---

## Architecture

```
┌─────────────────────────────────────────────────────┐
│                  Cortex-A9 (PS)                     │
│   L1 (32KB) → L2 PL310 (512KB) → DDR3 (1GB)        │
│                     ↑                               │
│              Snoop Control Unit (SCU)               │
│                 ↗           ↘                       │
│           ACP port        HP0 port                  │
│        (coherent)      (non-coherent)               │
└─────────────────────────────────────────────────────┘
              ↑                   ↑
         64-bit AXI          64-bit AXI
              ↑                   ↑
┌─────────────────────────────────────────────────────┐
│         AXI DMA (axi_dma_0)  +  matmul_0 (HLS)     │
│  DMA: MM2S→ACP  |  S2MM→HP0                        │
│  matmul_0: A,B reads via ACP | C write via HP0     │
│            B_T on dedicated BT AXI bundle           │
└─────────────────────────────────────────────────────┘
```

---

## Block Design

<img width="1866" height="830" alt="image" src="https://github.com/user-attachments/assets/8ff5c5ed-b9ac-430f-b26c-80f0721b432f" />

---

## DDR Memory Map

| Buffer  | Base Address | Size | Purpose |
|---------|-------------|------|---------|
| MAT_A   | 0x10000000  | 4 MB | Input matrix A |
| MAT_B   | 0x10400000  | 4 MB | Input matrix B |
| MAT_C   | 0x10800000  | 4 MB | Result matrix C |
| MAT_B_T | 0x10C00000  | 4 MB | B transposed scratch (HW accelerator internal) |

---

## Benchmark Modes

| Mode | Description |
|------|-------------|
| **ACP** | DMA loopback via coherent ACP port — no cache flush on source |
| **HP** | DMA loopback via non-coherent HP0 — explicit flush + invalidate |
| **SW** | N×N matrix multiply on ARM Cortex-A9 in software, cold-cache start |
| **MATMUL** | HLS tiled matmul_0 accelerator |

Mode is selected at build time via Vitis preprocessor symbol `MATMUL_MODE=1` (SW) or `MATMUL_MODE=2` (HW).

---

## Key Results

### 1. ACP vs HP0 — Coherent vs Non-Coherent Memory Path

ACP is consistently faster than HP0 because cache-warm source data is served directly by the SCU without a DDR round-trip. HP0 requires an explicit flush before transfer, adding overhead that grows with matrix size.

| N | ACP (µs) | HP (µs) | ACP/HP ratio |
|---|----------|---------|-------------|
| 32 | 33 | 57 | 0.58× |
| 64 | 112 | 212 | 0.53× |
| 128 | 429 | 828 | 0.52× |
| 256 | 1,698 | 3,207 | 0.53× |
| 512 | 6,773 | 10,656 | 0.64× |

ACP is ~1.5–1.9× faster than HP0 across all sizes.

---

### 2. SW vs HW Matrix Multiply

The HLS accelerator (`matmul_0`, FETCH_TILE=16, dedicated B_T AXI bundle) vs ARM Cortex-A9 software matmul:

| N | SW (µs) | HW (µs) | Speedup |
|---|---------|---------|---------|
| 32 | 2,421 | 686 | **3.53×** |
| 64 | 19,122 | 4,613 | **4.15×** |
| 128 | 152,664 | 33,544 | **4.55×** |
| 256 | 1,218,448 | 255,160 | **4.78×** |
| 512 | 9,740,419 | 1,993,182 | **4.89×** |

Speedup grows with N as DDR bandwidth utilisation improves with larger tiles. L1 hit rate is 100% for both SW and HW at large N — the bottleneck is DDR bandwidth, and the accelerator wins by fetching larger bursts and reusing data more efficiently from BRAM.

---

## HLS Accelerator Design

The `matmul_0` kernel (see `matmul/matmul.cpp`) runs in two stages:

**Stage 1 — Transpose B → B_T**  
Reads B row-by-row from DDR (sequential, burst-friendly via ACP), writes B_T to DDR scratch via a dedicated BT AXI bundle. Converting B's columns into rows of B_T makes the subsequent matmul access pattern sequential rather than strided.

**Stage 2 — Tiled multiply**  
Iterates over FETCH_TILE×FETCH_TILE blocks of A and B_T. Each block is loaded from DDR into on-chip BRAM. The inner MAC engine (TILE=4, fully unrolled — 16 multipliers in parallel) operates entirely from BRAM. Completed output tiles are written to C via HP0.

| Parameter | Value | Role |
|-----------|-------|------|
| `FETCH_TILE` | 16 | DDR fetch granularity — 16×16 floats per AXI round-trip |
| `TILE` | 4 | Compute sub-tile — 4×4 unrolled MAC engine (16 DSP48s) |
| `T_TILE` | 16 | Transpose tile size |
| `MAX_N` | 512 | Maximum supported matrix dimension |

**N must be a multiple of 16** (= max(FETCH_TILE, T_TILE)) for valid kernel operation.

### AXI Port Assignment

| Port | Bundle | Path | Reason |
|------|--------|------|--------|
| A | ACP | Coherent | Sequential row reads |
| B | ACP | Coherent | Sequential reads for transpose |
| B_T | BT (dedicated) | Separate bundle | Avoids write/read race with A,B on shared ACP |
| C | HP0 | Non-coherent | Bulk output writes |

---

## Correctness Checks

Three checks run at startup before the benchmark sweep:

| Check | What it tests |
|-------|--------------|
| SW at N=16 | Identity A × known B → C must equal B |
| HW at N=16,32,64,128,256,512 | Same identity test at every sweep size |
| SW vs HW cross-check at N=32 | Asymmetric non-trivial A and B; HW output must match SW reference within 8 ULP (rounding tolerance for different accumulation order) |

---

## Instrumentation

**ARM PMU (L1)** — CP15 coprocessor registers:  
- Event `0x03` — L1D cache miss · Event `0x04` — L1D cache access

**PL310 L2** — MMIO at `0xF8F02000`:  
- `DRREQ` (total read requests) · `DRHIT` (read hits) · Hit rate = DRHIT/DRREQ

**Timing** — Global Timer via `XTime_GetTime()`, 333 MHz (`COUNTS_PER_SECOND = 333333343`)

---

## Repository Structure

```
benchmark-app/
├── src/
│   ├── main.c               ← entry point, accelerator init, correctness checks
│   ├── benchmark.c          ← ACP, HP, SW, MATMUL sweep functions
│   ├── dma_smoke_test.c/.h  ← ACP and HP0 loopback validation
│   └── pmu.h                ← ARM PMU + PL310 L2 counter helpers
├── matmul/
│   ├── matmul.cpp           ← HLS accelerator source (Bhavana)
│   ├── matmul_tb.cpp        ← HLS testbench
│   └── README.md            ← HLS design notes and iteration history
├── analysis/
│   ├── plot_results.py      ← chart generator (latency, cache hit rates, speedup)
│   ├── results_with_sw2.txt ← latest SW matmul UART capture
│   ├── results_with_hw5_v2.txt ← latest HW matmul UART capture
│   ├── figures - no accelerator/        ← ACP + HP DMA baseline
│   ├── figures2 - sw accelerator/       ← ACP + HP + SW matmul
│   ├── figures3 - hw accelerator 1/     ← HW v1 untiled
│   ├── figures4-hwAccelerator2-tiling/  ← HW v2 TILE=4
│   ├── figures5-hwAccelerator3-tiling-v2/ ← HW v3 FETCH_TILE=8
│   ├── figures6-hwAccelerator4-FetchTile16-bT-AXI/ ← HW v4 FETCH_TILE=16
│   └── figures7-hwv4-vs-sw2/            ← latest: HW v5 vs SW
└── README.md
```

---

## DMA Smoke Tests

Three tests run at startup before the sweep:

| Test | Expected | Confirms |
|------|----------|----------|
| ACP loopback (N=256) | PASS | SCU serves dirty cache lines to DMA |
| HP0 loopback (N=256) | PASS | Flush + invalidate discipline works |
| Stale data (flush omitted) | FAIL | Omitting flush causes silent corruption on HP0 |

---

## References

| Document | ID |
|----------|----|
| Zynq-7000 SoC Technical Reference Manual | UG585 |
| ARM Cortex-A9 Technical Reference Manual | DDI0388 |
| ARM PL310 L2 Cache Controller TRM | DDI0246 |
| AXI DMA v7.1 Product Guide | PG021 |
| Zybo Z7-20 Reference Manual | Digilent |

---

## Acknowledgments

Debugging, documentation, and code were developed with assistance from Claude AI (Anthropic).
