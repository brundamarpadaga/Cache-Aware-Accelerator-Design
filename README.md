# Cache-Aware Accelerator Design: Benchmarking ACP vs HP on the Zynq-7 SoC

> Empirical benchmark comparing coherent (ACP) vs non-coherent (HP0) PS↔PL memory transfer paths on the Xilinx Zynq-7020 SoC, using ARM PMU hardware counters and PL310 L2 cache instrumentation.

**Board:** Digilent Zybo Z7-20 (XC7Z020-1CLG400C, 1GB DDR3)  
**Toolchain:** Vivado 2022.2 · Vitis 2022.2 · arm-none-eabi-gcc  

---

## About This Project

The Zynq-7020 exposes two distinct pathways for PL-to-PS memory transfers:

- **ACP (Accelerator Coherency Port)** — routes through the Snoop Control Unit (SCU), participates in the ARM MESI cache coherency protocol, and can read/write directly into the shared L2 cache. No explicit cache management needed.
- **HP Ports (High Performance Ports)** — bypass the SCU entirely, go straight to the DDR3 controller. Higher raw bandwidth (128-bit bus) but no cache coherency — requires explicit `Xil_DCacheFlushRange()` before PL reads and `Xil_DCacheInvalidateRange()` after PL writes, or stale data corruption occurs silently.

The central question: **when does coherent ACP access outperform the raw bandwidth of HP, and vice versa?**

The benchmark uses matrix multiplication as the workload, sweeping matrix size from 32×32 to 512×512 floats. Small matrices that fit in the 512KB L2 cache favour ACP — the PL reads data already warm in cache, avoiding DDR entirely. Large matrices exceeding the cache favour HP, where the wider DDR bus wins.

---

## Architecture

```
┌─────────────────────────────────────────────────────┐
│                  Cortex-A9 (PS)                     │
│   L1 (32KB) → L2 PL310 (512KB) → DDR3 (1GB)       │
│                     ↑                               │
│              Snoop Control Unit (SCU)               │
│                 ↗           ↘                       │
│           ACP port        HP0 port                  │
│        (coherent)      (non-coherent)               │
└───────────────────────────────────────────────────--┘
              ↑                   ↑
         64-bit AXI          64-bit AXI
              ↑                   ↑
┌─────────────────────────────────────────────────────┐
│              AXI DMA (PL fabric)                    │
│         MM2S → ACP   |   S2MM → HP0                │
│              ILA probes on both paths               │
└─────────────────────────────────────────────────────┘
```

---

## Repository Structure

```
project/
├── vivado/
│   ├── system.bd.tcl        ← block design Tcl export (version controlled)
│   ├── constraints.xdc      ← Zybo Z7-20 pin assignments
│   └── matmul_core.sv       ← PL accelerator (Member A, in development)
├── vitis/
│   ├── main.c               ← benchmark entry point + smoke test
│   └── pmu.h                ← ARM PMU (CP15) + PL310 L2 counter interface
├── analysis/
│   └── plot_results.py      ← latency and cache hit rate charts
└── README.md
```

---

<img width="1525" height="718" alt="image" src="https://github.com/user-attachments/assets/cb76e06e-88a4-4636-a039-9e09c2df7b6c" />


## DDR Memory Map

| Buffer  | Base Address | Size | Purpose          |
|---------|-------------|------|------------------|
| MAT_A   | 0x10000000  | 4MB  | Input matrix A   |
| MAT_B   | 0x10400000  | 4MB  | Input matrix B   |
| MAT_C   | 0x10800000  | 4MB  | Result matrix C  |

Buffers start at 256MB into DDR — clear of application code, stack, and heap. Safe for future Linux/U-Boot use on the same board.

---

## Instrumentation

### ARM PMU — L1 Cache (CP15 coprocessor registers, inline assembly)

| Counter | Event ID | Measures |
|---------|----------|----------|
| CTR0 | 0x03 | L1D cache miss (refill) |
| CTR1 | 0x04 | L1D cache access |

> Note: Cortex-A9 PMU events 0x14/0x16 (L2) return zero on Zynq — the PL310 is a standalone controller, not wired into the CPU PMU.

### PL310 L2 Cache — MMIO at 0xF8F02000

| Counter | Register Offset | Event (bits[7:2]) | Measures |
|---------|----------------|-------------------|----------|
| CTR0 | 0x208 | 0x3 << 2 = 0x0C | DRREQ — data read requests (total L2 accesses) |
| CTR1 | 0x204 | 0x2 << 2 = 0x08 | DRHIT — data read hits |

L2 hit rate = DRHIT / DRREQ × 100%

### Timing

Global Timer via `XTime_GetTime()` — runs at CPU_CLK/2 = 333MHz (`COUNTS_PER_SECOND = 333333343`).

---

## Benchmark Paths

### ACP (Coherent)
```
mat_fill(A, B)          ← data sits dirty in L1/L2
DMB barrier
START DMA → ACP mode    ← SCU snoops cache, PL gets fresh data
poll DONE
read result             ← no flush/invalidate needed
```

### HP0 (Non-Coherent)
```
mat_fill(A, B)
Xil_DCacheFlushRange(A) ← push dirty lines to DDR so PL sees them
Xil_DCacheFlushRange(B)
START DMA → HP mode     ← PL reads straight from DDR controller
poll DONE
Xil_DCacheInvalidateRange(C) ← discard stale PS cache copy
read result
```

Flush and invalidate overhead is deliberately included in measured latency — it is a real cost of the non-coherent approach.

---

## Progress

### Member A — PL Hardware (Vivado)

| Task | Status |
|------|--------|
| Vivado + Digilent board files installed | ✅ Complete |
| Zynq PS configured (DDR3, MIO, clocks) | ✅ Complete |
| ACP slave interface enabled (64-bit) | ✅ Complete |
| HP0 slave interface enabled (64-bit) | ✅ Complete |
| M_AXI_GP0 master interface enabled | ✅ Complete |
| FCLK_CLK0 at 100MHz, IRQ_F2P enabled | ✅ Complete |
| AXI DMA added (64-bit, 16-beat burst, no SG) | ✅ Complete |
| AXI Protocol Converters (AXI4→AXI3) for ACP and HP0 | ✅ Complete |
| Address map resolved (DMA @ 0x40400000) | ✅ Complete |
| Bitstream generated | ✅ Complete |
| ILA probes on AXI bus | ✅ Complete |
| .xsa exported with bitstream | ✅ Complete |
| Matrix multiply accelerator (matmul_core.sv) | 🔄 In development |
| ILA waveform captures (AXI burst, response codes) | ⬜ Pending accelerator |

### Member B — PS Software (Vitis)

| Task | Status |
|------|--------|
| Vitis platform from .xsa, bare-metal BSP | ✅ Complete |
| UART hello world (115200 8N1, COM6) | ✅ Complete |
| Global Timer confirmed (333333343 counts/sec) | ✅ Complete |
| ARM PMU init via CP15 inline assembly (MRC/MCR) | ✅ Complete |
| L1D miss/access counters validated | ✅ Complete |
| PL310 L2 counters via MMIO (0xF8F02000) | ✅ Complete |
| L2 event register bit-shift fix (bits[7:2]) | ✅ Complete |
| L2 counter reset fix (0x7 resets both CTR0+CTR1) | ✅ Complete |
| benchmark_coherent() and benchmark_noncoherent() written | ✅ Complete |
| mat_fill(), align_up(), ticks_to_us() helpers | ✅ Complete |
| plot_results.py post-processing script | ✅ Complete |
| DMA smoke test (1KB ACP transfer, verify integrity) | ⬜ Pending |
| DMA smoke test (1KB HP0 transfer + cache flush) | ⬜ Pending |
| Deliberate flush omission — confirm stale data | ⬜ Pending |
| Full benchmark sweep 32×32 to 512×512, CSV over UART | ⬜ Pending accelerator |
| Latency vs matrix size plot | ⬜ Pending data |
| L2 hit rate chart + crossover identification | ⬜ Pending data |

---

## Validated Results So Far

PMU and PL310 counters confirmed working on hardware with a 64KB test buffer (16384 floats):

```
L1 access delta   : 258,254
L1 miss delta     :   2,051   → matches 64KB / 32B cache line = 2048 cold misses ✓
L2 access (DRREQ) :   6,112
L2 hit    (DRHIT) :   2,048   → second loop pass finds data warm in L2 ✓
```

PL310 confirmed accessible:
```
PL310 Cache ID : 0x410000C8
PL310 Ctrl     : 0x00000001  (enabled)
```

---

## Key Bugs Fixed During Development

| Bug | Root Cause | Fix |
|-----|-----------|-----|
| L2 PMU events returning zero | PL310 is standalone on Zynq, not wired into Cortex-A9 PMU | Read PL310 via MMIO at 0xF8F02000 |
| PL310 CTR1 never resetting | Control register `0x3` only resets CTR0 (bit1); CTR1 reset is bit2 | Changed to `0x7` |
| PL310 counters stuck at zero despite reset | Event ID written directly to register; field is at bits[7:2] not bits[1:0] | Shift event ID left by 2: `(0x3U << 2)` |
| `COUNTS_PER_SECOND` printing as 10 | `xil_printf` does not support `%llu` | Cast to `unsigned long`, use `%lu` |
| Build crash with large static array | 64KB stack array overflows BSP linker script BSS allocation | Use pre-allocated MAT_A buffer at 0x10000000 |

---

## Key Architecture Concepts

- **MESI protocol** — Modified, Exclusive, Shared, Invalid cache line states managed by the SCU
- **Snoop Control Unit (SCU)** — hardware arbiter maintaining coherency between the two Cortex-A9 cores and the ACP port
- **Cache line allocation via ACP** — PL writes through ACP pre-warm the ARM L2 cache before the CPU reads the result
- **Cache flush discipline** — HP transfers require explicit flush before DMA reads and invalidate after DMA writes; omitting either causes silent stale data corruption
- **PL310 standalone** — on Zynq-7000 the L2 is a separate ARM PL310 controller, not integrated into the Cortex-A9 PMU; must be read via MMIO

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
