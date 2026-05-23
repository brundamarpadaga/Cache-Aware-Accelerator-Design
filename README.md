# Cache-Aware Accelerator Design: Benchmarking ACP vs HP on the Zynq-7 SoC

## About This Project

This project investigates how the Programmable Logic (PL) fabric of the Xilinx Zynq-7020 SoC can access the Processing System (PS) memory hierarchy — specifically whether it can leverage the ARM Cortex-A9's L2 cache rather than going directly to DDR3.

The Zynq-7 exposes two distinct pathways for PL-to-PS memory transfers:

- **ACP (Accelerator Coherency Port)** — routes through the Snoop Control Unit (SCU), participates in the ARM MESI cache coherency protocol, and can read/write directly into the shared L2 cache
- **HP Ports (High Performance Ports)** — bypass the SCU entirely, go straight to the DDR controller, offering higher raw bandwidth but no cache coherency

The central question is: **when does coherent access via ACP outperform the raw bandwidth of the HP ports, and vice versa?** The answer has direct implications for how hardware accelerators should be designed on heterogeneous SoCs.

The benchmark uses matrix multiplication as the workload, sweeping matrix size from 32×32 to 512×512 floats. Small matrices that fit in the 512KB L2 cache favour the ACP path — the PL reads and writes data that is already warm in cache, avoiding DDR entirely. Large matrices that exceed the cache favour the HP path, where the 128-bit bus width and higher DDR bandwidth win out.

**Board:** Digilent Zybo Z7-20 (XC7Z020-1CLG400C, 1GB DDR3)

---

## Expected Outcomes

By the end of the project the team will have:

- A working PS↔PL DMA system with both coherent (ACP) and non-coherent (HP0) transfer paths implemented and verified
- Empirical latency measurements for both paths across a range of matrix sizes, collected using ARM PMU hardware performance counters
- L2 cache hit/miss rate data showing how cache utilisation changes with matrix size
- ILA (Integrated Logic Analyzer) waveform captures showing AXI burst behaviour, transaction latency, and response codes on both paths
- A crossover plot identifying the exact matrix size at which the HP path's bandwidth advantage overtakes the ACP coherency advantage on this specific board
- Demonstrated understanding of the MESI coherency protocol at the hardware level — including what happens when the cache flush is deliberately omitted on the HP path

---

## Progress

### Environment Setup — Complete
- Vivado 2022.2 installed on Windows (Git Bash)
- Digilent board files cloned and installed for Zybo Z7-20
- Board confirmed in Vivado board selector (`xc7z020clg400-1`)
- JTAG cable drivers installed and verified — board programs successfully

### Vivado Block Design — Complete
- Zynq PS IP configured with board preset applied (DDR3 timing, MIO, clocks)
- ACP slave interface enabled (64-bit, coherent path)
- HP0 slave interface enabled (64-bit, non-coherent path)
- M_AXI_GP0 master interface enabled (PS controls DMA registers)
- FCLK_CLK0 running at 100 MHz
- IRQ_F2P interrupt input enabled
- AXI DMA IP added (64-bit data width, 16-beat burst, scatter-gather disabled)
- AXI Protocol Converters added (AXI4→AXI3) for both MM2S→ACP and S2MM→HP0 paths
- Address map resolved: DMA registers at 0x4040_0000, DDR accessible at 0x0000_0000
- Design validated with no errors
- Bitstream generated successfully

### Remaining Work

| Phase | Task | Status |
|---|---|---|
| Software setup | Export hardware (.xsa), create Vitis platform | Not started |
| Software setup | Bare-metal BSP, hello world + DMA init | Not started |
| Smoke test | 1KB buffer transfer via ACP, verify integrity | Not started |
| Smoke test | 1KB buffer transfer via HP0 with cache flush | Not started |
| Smoke test | Deliberate flush omission — confirm stale data | Not started |
| Smoke test | 100 consecutive transfers — confirm stability | Not started |
| Benchmark | Matrix multiply kernel, PMU counter harness | Not started |
| Benchmark | Sweep 32×32 to 512×512, log CSV over UART | Not started |
| Analysis | Plot latency vs matrix size, ACP vs HP0 | Not started |
| Analysis | L2 hit/miss rate chart, crossover identification | Not started |
| Report | Final write-up and demo | Not started |

---

## Team Division

### Member A — PL Hardware (Vivado)
Owns the block design, bitstream generation, and ILA-based waveform analysis during benchmark runs.

### Member B — PS Software (Vitis)
Owns the bare-metal C application, DMA driver wrappers, PMU counter harness, benchmark loop, and post-processing scripts.

**Shared milestones:** DMA smoke test (week 2), benchmark runs (week 5), final report (week 7).

---

## Repository Structure (Planned)

```
project/
├── vivado/
│   ├── system.bd.tcl       ← block design Tcl export (version controlled)
│   ├── constraints.xdc     ← Zybo Z7-20 pin assignments
│   └── matmul_core.sv      ← PL accelerator (Member A)
├── vitis/
│   ├── main.c              ← benchmark entry point (Member B)
│   ├── benchmark.c/.h      ← ACP and HP path wrappers
│   ├── pmu.c/.h            ← ARM PMU counter interface
│   └── memory_map.h        ← buffer base addresses
├── analysis/
│   └── plot_results.py     ← latency and cache hit rate charts
└── README.md
```

---

## Key Architecture Concepts

- **MESI protocol** — Modified, Exclusive, Shared, Invalid cache line states managed by the SCU
- **Snoop Control Unit (SCU)** — the hardware arbiter that maintains coherency between the two Cortex-A9 cores and the ACP port
- **Cache line allocation via ACP** — PL writes through ACP can pre-warm the ARM L2 cache before the CPU reads the result
- **Cache flush discipline** — HP transfers require explicit `Xil_DCacheFlushRange()` before DMA reads and `Xil_DCacheInvalidateRange()` after DMA writes; omitting either causes silent data corruption

---

## Tools and References

| Tool | Version | Purpose |
|---|---|---|
| Vivado ML Standard | 2022.2 | Block design, synthesis, bitstream |
| Vitis IDE | 2022.2 | Bare-metal C development |
| Digilent vivado-boards | master | Zybo Z7-20 board preset |
| Git Bash (MINGW64) | — | Version control on Windows |

**Key documents:**
- Zynq-7000 SoC Technical Reference Manual (UG585) — ACP chapter, L2 cache chapter
- AXI DMA v7.1 Product Guide (PG021)
- Zybo Z7-20 Reference Manual (Digilent)
