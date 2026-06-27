figures2/ — Benchmark Results: ACP + HP0 + SW Matmul Sweep
=============================================================
Board:   Digilent Zybo Z7-20 (XC7Z020-1CLG400C)
Log:     results_with_sw  (UART capture, 115200 8N1)
Authors: Brunda Marpadaga, Bhavana Marpadaga

This folder contains plots from the second benchmark run, which added a
software matrix multiply (SW) mode alongside the existing ACP and HP0
DMA loopback benchmarks. The ARM CPU performs the full N×N multiply in
software — no hardware accelerator yet.

Files
-----
latency.png      Time (µs) vs matrix side N for all three modes.
                   ACP — DMA loopback via ACP, no cache flush needed (SCU snoops)
                   HP  — DMA loopback via HP0, includes SRC flush + DST invalidate
                   SW  — Software N×N matmul on ARM Cortex-A9, cold-cache start

l1_hit_rate.png  L1 D-cache hit rate (%) vs N.
                   ARM PMU events: 0x03 (miss count) / 0x04 (access count)

l2_hit_rate.png  PL310 L2 cache hit rate (%) vs N.
                   Derived from MMIO counters at 0xF8F02000 (DRREQ / DRHIT)

speedup.png      HP and SW elapsed time normalised to ACP elapsed time.
                   Ratio > 1 means ACP is faster than that mode.

l1_misses.png    Raw L1 D-cache miss count vs N for each mode.

Comparison Context
------------------
analysis/figures/   DMA-only baseline (ACP + HP0, no matmul)
analysis/figures2/  This folder — adds SW matmul for comparison
analysis/figures3/  (Planned) HW matmul results once Bhavana's Vitis HLS
                    accelerator (matmul_0) is integrated via the new XSA.

What to expect in figures3
---------------------------
The hardware matmul accelerator (matmul_0) reads matrix A via ACP and
writes matrix C via HP0, independently of the DMA. It runs at 50 MHz
(FCLK_CLK0, Fmax 136 MHz). The MATMUL benchmark times from ap_start
to ap_done including the DST cache invalidate, with the same PMU/PL310
counters active. The SW curve in this folder is the baseline to beat.
