# Cache-Aware Accelerator Design: Benchmarking ACP vs HP on the Zynq-7 SoC

> Empirical benchmark comparing coherent (ACP) vs non-coherent (HP0) PS↔PL memory transfer paths on the Xilinx Zynq-7020 SoC, extended with a cache-aware tiled matrix multiply HLS accelerator.

**Board:** Digilent Zybo Z7-20 (XC7Z020-1CLG400C, 1GB DDR3)  
**Toolchain:** Vivado 2022.2 · Vitis 2022.2 · arm-none-eabi-gcc  
**Authors:** Brunda Marpadaga · Bhavana Marpadaga

---

## About This Project

The Zynq-7020 exposes two distinct pathways for PL-to-PS memory transfers:

- **ACP (Accelerator Coherency Port)** — routes through the Snoop Control Unit (SCU), participates in the ARM MESI cache coherency protocol, and can read/write directly into the shared L2 cache. No explicit cache management needed.
- **HP Ports (High Performance Ports)** — bypass the SCU entirely, go straight to the DDR3 controller. Higher raw bandwidth (128-bit bus) but no cache coherency — requires explicit `Xil_DCacheFlushRange()` before PL reads and `Xil_DCacheInvalidateRange()` after PL writes, or stale data corruption occurs silently.

The project benchmarks four modes across matrix sizes N=32 to N=512:

| Mode | Description |
|------|-------------|
| **ACP** | DMA loopback via ACP — coherent, SCU snoops PS cache |
| **HP** | DMA loopback via HP0 — non-coherent, explicit flush/invalidate |
| **SW** | Software N×N matmul on ARM Cortex-A9, cold-cache start |
| **MATMUL** | HLS tiled matrix multiply accelerator (matmul_0) |

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
│  DMA: MM2S→ACP | S2MM→HP0                          │
│  matmul_0: A,B,B_T reads via ACP | C write via HP0 │
└─────────────────────────────────────────────────────┘
```

---

## Block Design

<img width="1866" height="830" alt="image" src="https://github.com/user-attachments/assets/8ff5c5ed-b9ac-430f-b26c-80f0721b432f" />

### Port Roles

| Port | Role |
|------|------|
| `M_AXI_GP0` | ARM commands the DMA and matmul_0 (control plane) |
| `S_AXI_ACP` | DMA / matmul_0 reads with cache coherency via SCU |
| `S_AXI_HP0` | DMA / matmul_0 writes directly to DDR, bypassing SCU |
| `FCLK_CLK0` | 50 MHz fabric clock driving all PL logic |
| `IRQ_F2P` | DMA signals transfer completion to ARM GIC |

---

## Repository Structure

```
benchmark-app/
├── src/
│   ├── main.c               ← entry point, accelerator init, correctness check
│   ├── benchmark.c          ← ACP, HP, SW, MATMUL sweep loops
│   ├── benchmark.h
│   ├── dma_smoke_test.c/.h  ← ACP and HP0 DMA loopback validation
│   └── pmu.h                ← ARM PMU (CP15) + PL310 L2 counter interface
├── matmul/
│   ├── matmul.cpp           ← HLS accelerator source (Bhavana)
│   ├── matmul_tb.cpp        ← HLS testbench
│   └── README.md            ← HLS design notes
├── analysis/
│   ├── plot_results.py      ← latency and cache hit rate chart generator
│   ├── dma_baseline.log     ← UART capture: ACP + HP DMA loopback only
│   ├── results_with_sw      ← UART capture: ACP + HP + SW matmul
│   ├── results_with_hw.log  ← UART capture: HW accelerator v1 (untiled)
│   ├── results_with_hw3.txt ← UART capture: HW accelerator v2 (tiled) ← latest
│   ├── figures - no accelerator/     ← plots: ACP + HP baseline
│   ├── figures2 - sw accelerator/    ← plots: ACP + HP + SW
│   ├── figures3 - hw accelerator 1/  ← plots: HW v1 (untiled)
│   └── figures4-hwAccelerator2-tiling/ ← plots: HW v2 (tiled) + comparison
└── README.md
```

---

## DDR Memory Map

| Buffer  | Base Address | Size | Purpose |
|---------|-------------|------|---------|
| MAT_A   | 0x10000000  | 4 MB | Input matrix A |
| MAT_B   | 0x10400000  | 4 MB | Input matrix B |
| MAT_C   | 0x10800000  | 4 MB | Result matrix C |
| MAT_B_T | 0x10C00000  | 4 MB | B transposed scratch (HW accelerator) |

Buffers start at 256 MB into DDR — clear of application code, stack, and heap.

---

## HLS Accelerator — matmul_0

The `matmul_0` HLS accelerator (see `matmul/matmul.cpp`) implements cache-aware tiled matrix multiplication in two stages:

1. **Transpose** — reads B row-by-row (sequential AXI bursts via ACP), writes B_T column-by-column to DDR scratch via HP0. Converts the strided column access of a naive matmul into sequential row access.

2. **Tiled multiply** — iterates over TILE×TILE blocks of A and B_T, loads each block into on-chip BRAM, then performs all multiply-accumulate for that block from BRAM only (zero DDR reads during compute). Writes completed output tiles to C via HP0.

**Tile size:** `TILE=4` (configurable). Each tile occupies 4×4×4 = 64 bytes of BRAM. Three tiles (a_tile, b_tile, c_tile) = 192 bytes total — well within the XC7Z020's ~600 KB BRAM budget.

### AXI Port Assignment

| Port | Bundle | Path | Reason |
|------|--------|------|--------|
| A | ACP | Coherent | Reads data warm in PS L1/L2 cache |
| B | ACP | Coherent | Sequential burst reads for transpose |
| B_T | ACP | Coherent | Reads after HW writes — SCU snoops |
| C | HP0 | Non-coherent | Bulk write output, PS invalidates after |
| N, return | s_axilite | — | Control register interface |

### Cache Coherency Protocol (PS side)

Before calling `XMatmul_Start()`:
1. Flush A and B to DDR — HW reads via ACP, but A/B were written by PS
2. Flush C region (zeroed) to DDR — HW writes C via HP0 (non-coherent)
3. **Invalidate B_T region** — HW writes B_T via HP0 then reads back via ACP; without invalidate the SCU serves stale PS cache lines instead of the fresh HP0-written DDR data

After `XMatmul_IsDone()`:
4. Invalidate C region — PS must not read from its stale cache copy of C

---

## Benchmark Results

### HW v1 (untiled) vs HW v2 (tiled)

Tiling delivers **11–14× speedup** over the untiled implementation by eliminating redundant DDR reads during the inner multiply loop.

| N | HW v1 (µs) | HW v2 (µs) | Speedup |
|---|-----------|-----------|---------|
| 32 | 22,672 | 1,985 | **11.4×** |
| 64 | 180,876 | 14,776 | **12.2×** |
| 128 | 1,445,154 | 114,070 | **12.7×** |
| 256 | 11,557,458 | 896,958 | **12.9×** |
| 512 | 101,325,293 | 7,113,180 | **14.2×** |

### HW v2 vs SW Matmul

The tiled HW accelerator beats the ARM Cortex-A9 software matmul at every matrix size:

| N | SW (µs) | HW v2 (µs) | HW speedup |
|---|---------|-----------|------------|
| 32 | 2,421 | 1,985 | **1.22×** |
| 64 | 19,121 | 14,776 | **1.29×** |
| 128 | 152,666 | 114,070 | **1.34×** |
| 256 | 1,218,443 | 896,958 | **1.36×** |
| 512 | 9,740,454 | 7,113,180 | **1.37×** |

### Cache Behaviour (HW v2)

- **L1 hit rate: 100%** — tiled access keeps the working set in L1 during inner loops
- **L2 hit rate: ~50%** — B_T rows reused across the tile, hitting L2 on repeated access
- This confirms the tiling strategy is effective: the accelerator is compute-bound rather than memory-bandwidth-bound for the inner tile loop

### ACP vs HP DMA Baseline

ACP is consistently ~0.52–0.64× HP latency, confirming the coherency advantage for cache-warm data.

---

## Instrumentation

### ARM PMU — L1 Cache (CP15 coprocessor registers)

| Counter | Event ID | Measures |
|---------|----------|----------|
| CTR0 | 0x03 | L1D cache miss (refill) |
| CTR1 | 0x04 | L1D cache access |

### PL310 L2 Cache — MMIO at 0xF8F02000

| Counter | Register Offset | Event | Measures |
|---------|----------------|-------|----------|
| CTR0 | 0x208 | `0x3 << 2` | DRREQ — total L2 data read requests |
| CTR1 | 0x204 | `0x2 << 2` | DRHIT — data read hits |

L2 hit rate = DRHIT / DRREQ × 100%

### Timing

Global Timer via `XTime_GetTime()` — runs at CPU_CLK/2 = 333 MHz (`COUNTS_PER_SECOND = 333333343`).

---

## Compile-Time Mode Selection

The benchmark mode is selected at build time via a preprocessor symbol in Vitis project settings:

| Symbol | Value | Behaviour |
|--------|-------|-----------|
| `MATMUL_MODE` | `1` | SW matmul (ARM CPU, no accelerator needed) |
| `MATMUL_MODE` | `2` | HW matmul (requires matmul_0 in XSA) |

Add the symbol under **Project → Properties → C/C++ Build → Settings → Symbols**.

---

## DMA Smoke Tests

Three tests run before the full benchmark sweep:

| Test | Expected | What It Confirms |
|------|----------|-----------------|
| ACP loopback (N=256, 256 KB) | PASS | SCU correctly serves dirty cache lines to DMA |
| HP0 loopback (N=256, 256 KB) | PASS | Flush + invalidate discipline works |
| Stale data (flush omitted) | FAIL | Omitting flush causes silent corruption on HP0 |

---

## Key Bugs Fixed During Development

| Bug | Root Cause | Fix |
|-----|-----------|-----|
| L2 PMU events returning zero | PL310 is standalone on Zynq, not wired into Cortex-A9 PMU | Read PL310 via MMIO at 0xF8F02000 |
| PL310 CTR1 never resetting | `0x3` only resets CTR0; CTR1 reset is bit2 | Changed to `0x7` |
| PL310 counters stuck at zero | Event ID written directly; field is at bits[7:2] | Shift event ID: `(0x3U << 2)` |
| DMA SimpleTransfer status=15 | AXI DMA buffer length register only 14-bit | Widened to 23-bit in Vivado |
| DMA hanging after transfer | MM2S started before S2MM armed | Start S2MM before MM2S always |
| ACP reading DDR instead of cache | AXI user signals (ARCACHE/AWCACHE) driven zero → SCU bypass | Added Constant IP blocks driving correct cache attributes |
| MATMUL wrong register offsets | Report showed C@0x20, N@0x28; actual xmatmul.h has B_T@0x28, C@0x34, N@0x40 | Fixed Set_B_T/Set_C/Set_N calls to match generated driver |
| MATMUL NaN results (B_T) | HW writes B_T via HP0 then reads via ACP; stale PS cache lines served by SCU | Added `Xil_DCacheInvalidateRange(B_T)` before XMatmul_Start |

---

## References

| Document | ID | Relevant Sections |
|----------|----|-------------------|
| Zynq-7000 SoC Technical Reference Manual | UG585 | Chapter 22 (ACP), Chapter 3 (PS overview) |
| ARM Cortex-A9 Technical Reference Manual | DDI0388 | Chapter 4 (CP15), Chapter 11 (PMU) |
| ARM PL310 L2 Cache Controller TRM | DDI0246 | Chapter 3 (event counters, register map) |
| ARM Architecture Reference Manual ARMv7 | DDI0406 | A8.8.108 (MCR), A8.8.110 (MRC) |
| AXI DMA v7.1 Product Guide | PG021 | Simple DMA mode, register map |
| Zybo Z7-20 Reference Manual | — | Digilent board documentation |

## Acknowledgments

This project's debugging process, documentation, and code were developed with assistance from Claude AI (Anthropic) — particularly for cache coherency analysis, PL310 register configuration, CP15 inline assembly, and HLS pragma explanation.
