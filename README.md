# Dual-Core AMP Benchmark: HW Accelerator vs SW Matrix Multiply on Zynq-7020

> Asymmetric multiprocessing benchmark on Xilinx Zynq-7020 running both ARM Cortex-A9 cores simultaneously: Core 0 drives an HLS matrix multiply accelerator under FreeRTOS; Core 1 runs a software matrix multiply baseline on bare-metal. Both measure L1 and L2 cache behaviour per-core while running concurrently.

**Board:** Digilent Zybo Z7-20 (XC7Z020-1CLG400C, 1 GB DDR3)  
**Toolchain:** Vivado 2022.2 · Vitis 2022.2 · FreeRTOS 10 · arm-none-eabi-gcc  
**Authors:** Brunda Marpadaga · Bhavana Marpadaga  
**Branch:** `feature/freertos-dual-core` (standalone — not merged into main)

---

## What This Does

The benchmark runs both cores at the same time and prints interleaved CSV rows to UART:

```
MATMUL,N,us,L1acc=...,L1miss=...,L2req=...,L2hit=...,L2pct=...
SW_CORE1,N,us,L1acc=...,L1miss=...,L1pct=...,L2req=...,L2hit=...,L2pct_approx=...
```

- **`MATMUL`** — Core 0 measurement: time for `matmul_0` HLS accelerator to complete one N×N multiply, plus the Core 0 L1 PMU and shared L2 counters bracketing that run.
- **`SW_CORE1`** — Core 1 measurement: time for the ARM i-k-j software loop to complete one N×N multiply, plus Core 1's own L1 PMU and the shared L2 counters.

Both sweeps run sizes N = {32, 64, 128, 256, 512} × 5 repetitions. Total wall time ≈ 56 seconds (limited by the slower SW core; the HW core finishes inside the same window).

---

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│  Core 0 — Cortex-A9 — FreeRTOS                              │
│                                                             │
│  TaskHWMatmul                   TaskPrefill                 │
│  ├─ start matmul_0 accelerator  └─ fill staging buffers     │
│  ├─ poll/yield until ap_done       while accelerator runs   │
│  ├─ read L1 PMU (CP15)                                      │
│  └─ read L2 MMIO (0xF8F02000)                               │
│                    │                                        │
│             SEV → CORE1_CMD_RUN (shared DDR flag)           │
└─────────────────────────────────────────────────────────────┘
              ACP (coherent)    HP0 (non-coherent)
                    │                  │
         ┌──────────┴──────────────────┘
         │    matmul_0 (HLS, PL)
         │    reads A, B via ACP → L2 (shared with CPUs)
         │    writes C via HP0
         └──────────────────────────────────────────────────
┌─────────────────────────────────────────────────────────────┐
│  Core 1 — Cortex-A9 — bare-metal standalone                 │
│                                                             │
│  main()                                                     │
│  ├─ poll CORE1_CMD until CORE1_CMD_RUN                      │
│  ├─ for each N: i-k-j SW matmul loop                        │
│  ├─ read L1 PMU (own CP15 — private to Core 1)              │
│  └─ read L2 MMIO (shared aggregate)                         │
└─────────────────────────────────────────────────────────────┘
```

The two cores share one L2 PL310 cache (512 KB). L1 caches (32 KB each) are private. The SCU (Snoop Control Unit) maintains cache coherency and mediates the LDREX/STREX inter-core UART spinlock.

---

## Results

### Timing and Speedup

| N | SW Core 1 (µs) | HW Core 0 (µs) | Speedup |
|---|---------------|----------------|---------|
| 32 | 2,420 | 678 | **3.57×** |
| 64 | 19,121 | 4,587 | **4.17×** |
| 128 | 152,722 | 33,414 | **4.57×** |
| 256 | 1,218,858 | 254,583 | **4.79×** |
| 512 | 9,741,659 | 1,990,307 | **4.89×** |

Median of 5 repetitions. Speedup grows with N as the accelerator's DDR burst efficiency improves relative to the scalar ARM loop.

### Cache Behaviour

| Core | Counter | N=128 | N=512 | Notes |
|------|---------|-------|-------|-------|
| Core 0 (HW) | L1 accesses | ~3.8 M | ~228 M | FreeRTOS polling overhead; no matmul L1 activity |
| Core 0 (HW) | L1 misses | 0–3 | 0–3 | Confirms Core 0 is idle during accelerator |
| Core 0 (HW) | L2 hit rate | 37–48% | 36% | ACP reads; drops when Core 1 also active |
| Core 1 (SW) | L1 hit rate | 99% | 99% | i-k-j reuse keeps working set in L1 |
| Core 1 (SW) | L2 hit rate | 39–48% | 34–35% | Shared L2 — noisier under dual-core load |

**L1 values are per-core accurate** (ARM CP15 PMU, private registers).  
**L2 values are aggregate** — the PL310 event counters count traffic from both cores and the ACP accelerator reads. The `L2pct_approx` label on SW_CORE1 rows reflects this. L2 hit rates are lower than the single-core baseline because both cores thrash L2 simultaneously.

---

## DDR Memory Map

| Region | Base | Size | Owner |
|--------|------|------|-------|
| MAT_A (active) | `0x10000000` | 4 MB | matmul_0 reads (ACP) |
| MAT_B (active) | `0x10400000` | 4 MB | matmul_0 reads (ACP) |
| MAT_C | `0x10800000` | 4 MB | matmul_0 writes (HP0) |
| MAT_B_T | `0x10C00000` | 4 MB | matmul_0 transpose scratch |
| CORE1_COMM | `0x10F00000` | 20 B | inter-core flags + UART spinlock |
| SW_A | `0x11000000` | 4 MB | Core 1 input A |
| SW_B | `0x11400000` | 4 MB | Core 1 input B |
| SW_C | `0x11800000` | 4 MB | Core 1 result C |
| MAT_A_NEXT (staging) | `0x14000000` | 4 MB | TaskPrefill writes |
| MAT_B_NEXT (staging) | `0x14400000` | 4 MB | TaskPrefill writes |
| Core 1 code / data | `0x20000000` | 512 MB | sw_matmul ELF |

### Inter-Core Communication Region (`0x10F00000`)

| Offset | Field | Size | Description |
|--------|-------|------|-------------|
| `+0x00` | `CORE1_CMD` | 4 B | `0xDEAD0001` = run; `0x0` = idle |
| `+0x04` | `CORE1_STATUS` | 4 B | `0xCAFE0001` = done; `0x0` = busy |
| `+0x08` | `CORE1_ELAPSED_US` | 8 B | Core 1 total sweep time |
| `+0x10` | `UART_LOCK` | 4 B | inter-core spinlock (LDREX/STREX) |

---

## Key Design Decisions

### Double-buffered prefill
While `matmul_0` processes size N, `TaskPrefill` fills staging buffers for size N+1. `TaskHWMatmul` swaps active↔staging pointers after each size completes. This hides the matrix initialisation cost on Core 0 and keeps Core 0 busy while waiting.

### Per-core PMU instrumentation
ARM CP15 PMU registers (`c9`) are per-core — Core 0 cannot access Core 1's counters and vice versa. Each core brackets its own work:
```
pmu_reset_counters() + l2_reset()
XTime_GetTime(&t0) → workload → XTime_GetTime(&t1)
pmu_read_all() + l2_read()
```
The PL310 L2 MMIO (`0xF8F02000`) is shared MMIO — any core can read it, but it counts aggregate traffic from all sources (both CPUs + ACP DMA).

### Inter-core UART spinlock (LDREX/STREX)
Both cores share one UART. Without coordination, simultaneous `xil_printf` calls interleave characters in the 64-byte TX FIFO. The fix uses ARM exclusive load/store:

```c
// Acquire
do {
    do { ldrex got, [lock]; } while (got != 0);
    strex st, 1, [lock];
} while (st != 0);
dmb;

// Release
dmb;
*lock = 0;
```

The Cortex-A9 SCU tracks exclusive reservations at cache-line granularity. When Core 1 does `STREX` to a line that Core 0 holds exclusive, the SCU clears Core 0's reservation and Core 0's `STREX` returns 1 (fail), forcing a retry. This is the standard AMP alternative to OS mutexes — no RTOS involvement required on either side.

### Two-project AMP in Vitis
The Zynq boot ROM WFE loop at `0xFFFFFFF0` only works for SD/QSPI boot, not JTAG. Under JTAG, the debugger halts CPU1 before it reaches the loop. The solution is two separate application projects under one system project:

```
freertos-dualcore-matmul_system/
├── freertos-dualcore-matmul  (CPU0, FreeRTOS BSP)
└── sw_matmul                 (CPU1, standalone BSP, -DUSE_AMP=1)
```

The system project debugger loads both ELFs and releases both cores.

---

## Vitis 2022.2 Setup (new machine)

1. **Import XSA** — create a platform from `hardware/system_wrapper.xsa`.

2. **Add CPU1 domain** — in the platform, add a standalone domain targeting `ps7_cortexa9_1`. In BSP settings, add preprocessor symbol `-DUSE_AMP=1` (prevents the standalone BSP from disabling caches on CPU1, which would break AMP).

3. **Create CPU0 app** — new FreeRTOS application on `ps7_cortexa9_0`. Import `src/freertos_main.c` and `src/pmu.h`. Add preprocessor symbol `MATMUL_MODE=2` (selects HW accelerator path).

4. **Create CPU1 app** — new standalone application on `ps7_cortexa9_1` **inside the same system project** as the CPU0 app. Import `src/core1_entry.c` and `src/pmu.h`. Replace the generated `lscript.ld` in the app's `src/` with `src/lscript_core1.ld` from this repo (sets `ps7_ddr_0 ORIGIN = 0x20000000` to avoid overlapping Core 0).

5. **Debug** — launch from the system project's debug configuration; it loads both ELFs and starts both cores under one JTAG session.

---

## Repository Structure

```
benchmark-app/
├── src/
│   ├── freertos_main.c      ← Core 0: FreeRTOS TaskHWMatmul + TaskPrefill
│   ├── core1_entry.c        ← Core 1: bare-metal SW matmul sweep
│   ├── pmu.h                ← ARM L1 PMU + PL310 L2 MMIO + inter-core UART lock
│   ├── lscript_core1.ld     ← Core 1 linker script (ORIGIN = 0x20000000)
│   ├── main.c               ← legacy single-core entry (not used on this branch)
│   ├── benchmark.c/.h       ← legacy ACP/HP/SW sweep (not used on this branch)
│   └── dma_smoke_test.c/.h  ← legacy DMA loopback tests (not used on this branch)
├── matmul/
│   ├── matmul.cpp           ← HLS accelerator source (Bhavana)
│   ├── matmul_tb.cpp        ← HLS testbench
│   └── README.md            ← HLS design notes and iteration history
├── hardware/
│   ├── system_wrapper.xsa   ← Vivado-exported platform (import into Vitis)
│   └── system_bd.tcl        ← block design TCL (re-run in Vivado if XSA is lost)
├── analysis/
│   ├── plot_results.py      ← chart generator (latency, L1/L2 hit rates, speedup)
│   └── results_with_*/      ← UART capture logs from each benchmark run
└── README.md                ← this file
```

---

## HLS Accelerator

The `matmul_0` kernel (`matmul/matmul.cpp`) runs in two stages:

**Stage 1 — Transpose B → B_T**  
Reads B sequentially from DDR via ACP, writes B_T to a dedicated DDR scratch buffer via the BT AXI bundle. Converts column access in the multiply into sequential row access.

**Stage 2 — Tiled multiply**  
Iterates over FETCH_TILE×FETCH_TILE blocks of A and B_T. Each block is loaded from DDR into on-chip BRAM. The inner MAC engine (TILE=4, fully unrolled — 16 DSP48s in parallel) operates entirely from BRAM. Output tiles are written to C via HP0.

| Parameter | Value | Role |
|-----------|-------|------|
| `FETCH_TILE` | 16 | DDR fetch granularity — 16×16 floats per AXI round-trip |
| `TILE` | 4 | Compute sub-tile — 4×4 MAC engine (16 DSP48s in parallel) |
| `MAX_N` | 512 | Maximum supported matrix dimension |

N must be a multiple of 16 for valid operation.

| AXI Port | Bundle | Path | Reason |
|----------|--------|------|--------|
| A | default | ACP — coherent | Sequential row reads |
| B | default | ACP — coherent | Sequential reads for transpose |
| B_T | BT (dedicated) | separate AXI | Avoids write/read race with A, B |
| C | HP0 | non-coherent | Bulk output writes |

---

## Instrumentation

**ARM PMU — L1 (CP15, per-core)**

| Register | What it tracks |
|----------|---------------|
| Event `0x03` — L1D miss | L1 data cache misses |
| Event `0x04` — L1D access | L1 data cache accesses |

**PL310 — L2 (MMIO `0xF8F02000`, shared)**

| Counter | Event | Meaning |
|---------|-------|---------|
| CTR0 | `DRREQ` (`0x0C`) | Total L2 data read requests |
| CTR1 | `DRHIT` (`0x08`) | L2 data read hits |

Both counters are reset with `l2_reset()` before each measurement bracket, but they count aggregate traffic from all cores and the ACP port. Under dual-core load, these values are not directly comparable to single-core L2 measurements.

**Timing** — ARM global timer via `XTime_GetTime()`, 333 MHz.

---

## References

| Document | ID |
|----------|----|
| Zynq-7000 SoC Technical Reference Manual | UG585 |
| ARM Cortex-A9 Technical Reference Manual | DDI0388 |
| ARM PL310 Level-2 Cache Controller TRM | DDI0246 |
| Zybo Z7-20 Reference Manual | Digilent |

---

## Acknowledgments

Debugging, documentation, and portions of the code were developed with assistance from Claude AI (Anthropic).
