# Matmul Hardware Accelerator — Development README

Vitis HLS matrix-multiply accelerator for the Cache-Aware Accelerator Design
project on the Zybo Z7-20 (XC7Z020-1CLG400C). This document tracks the
accelerator's design history, the problems hit along the way, and the
current state of the kernel and its Vivado integration.

Repo: https://github.com/brundamarpadaga/Cache-Aware-Accelerator-Design

---

## 1. Goal

Add a PL-side matrix-multiply accelerator that reads matrices A and B from
DDR via the coherent ACP port, computes C = A × B in hardware, and writes
C back via the non-coherent HP0 port — then compare its performance against
the existing software matmul and DMA-loopback benchmarks (`figures2`).

---

## 2. Starting Point

Before this accelerator work began, the project already had:

- Vivado block design with Zynq PS, AXI DMA, two AXI protocol converters,
  and a working ACP/HP0 coherency path
- `const_arcache` / `const_aruser` fix forcing ACP signals to `0xF` so the
  SCU actually snoops (this was the original cache-coherency bug fix)
- `figures2` benchmark: ACP vs HP0 vs software matmul, collected and
  analyzed — software matmul scales as N³ and is ~1440× slower than DMA
  transfer at N=512, motivating the need for a hardware accelerator

---

## 3. The Naive Memory Access Pattern (and why it's slow)

Before any optimization, it helps to look at what a textbook matrix
multiply actually does to memory, since that's what motivated every
change that follows.

```
C[i][j] = sum over k of  A[i][k] * B[k][j]
```

To compute a single output element `C[i][j]`, the hardware needs **row i
of A** and **column j of B**:

```
A row i:        [ A[i][0]  A[i][1]  A[i][2]  ...  A[i][N-1] ]
B column j:      A[0][j]
                  A[1][j]
                  A[2][j]
                   ...
                  A[N-1][j]
```

In the most direct translation of the math into code, every single
multiply-accumulate re-fetches both operands straight from DDR:

```cpp
for (int i = 0; i < N; i++)
    for (int j = 0; j < N; j++) {
        float sum = 0;
        for (int k = 0; k < N; k++)
            sum += A[i*N+k] * B[k*N+j];   // both read from DDR every time
        C[i*N+j] = sum;
    }
```

The problem is **reuse**. Row i of A is needed again for every value of
`j` (N times), and column j of B is needed again for every value of `i`
(N times) — but nothing is kept around between iterations. Once
`C[i][j]` is computed, the row and column data that produced it is
simply discarded, and the next `(i,j)` pair re-fetches data that, in
many cases, was already in memory moments earlier:

```
compute C[0][0]:  fetch A row 0, fetch B col 0  →  discard both
compute C[0][1]:  fetch A row 0 AGAIN, fetch B col 1  →  discard both
compute C[0][2]:  fetch A row 0 AGAIN, fetch B col 2  →  discard both
...
```

A row of A ends up being re-read from DDR N times over the course of
computing one row of C, and the same is true for every column of B.
Across the full matrix this naive pattern requires on the order of
**N³ DDR reads** total — for N=512 that's well over 100 million reads
for a problem that only has N²=262,144 output elements and N²
multiply-accumulates per row. Almost all of that DDR traffic is pure
waste: re-fetching data that was already on-chip a few cycles earlier.

This is the core drawback that shaped the whole accelerator design: **if
nothing is cached locally, the same data gets pulled off DDR over and
over, and DDR bandwidth — not compute — becomes the bottleneck.**

### Motivation for `a_local`

The first fix is the simplest possible form of reuse: since row `i` of A
is reused N times (once per column of C in that row), pull the entire
row into on-chip BRAM **once**, then reuse it from BRAM for all N
multiply-accumulates instead of re-reading it from DDR each time:

```cpp
float a_local[N];
for (int k = 0; k < N; k++)
    a_local[k] = A[i*N+k];     // ONE pass over row i, straight into BRAM

for (int j = 0; j < N; j++) {
    float sum = 0;
    for (int k = 0; k < N; k++)
        sum += a_local[k] * B[k*N+j];   // A now comes from BRAM, not DDR
    C[i*N+j] = sum;
}
```

This drops A's DDR reads from N³ down to N² — each row is fetched once
and reused N times from BRAM. It does nothing for B yet (B is still
read column-wise, straight from DDR, every single multiply), which is
exactly the bottleneck the next several iterations had to work through.
`a_local` was the starting point — v1 below.

---

## 3a. Kernel Iteration History

### v1 — Naive, `a_local` only

```cpp
a_local[k] = A[i*N+k];               // row of A cached in BRAM
sum += a_local[k] * B[k*N+j];        // B read directly from DDR
```

II=5, Fmax=136MHz in HLS. On hardware this was the first integrated
version (`figures3`): **10× slower than software matmul** at N=512
(~1s vs 97ms). Root cause: `B[k*N+j]` is column-strided in memory —
DDR addresses jump by N×4 bytes every read — so AXI cannot burst these
transfers. N³ individual non-burst reads dominate the runtime.

### v2 — Accumulator array (modulo / counter indexing)

Tried `float acc[5]` with a runtime index to break a loop-carried
dependency on `sum`. Any runtime-indexed array write synthesizes as a
mux in series with the floating-point adder:

```
critical path: acc_idx load → mux (1.95ns) → fadd (7.26ns) = 9.2ns > 7.3ns budget
```

Result: II capped at 4, Fmax dropped to ~108MHz. Did not fix the
underlying strided-B problem either — abandoned.

### v3 — Pipeline the `j` loop, let `k` auto-unroll

```cpp
for (int j = 0; j < N; j++) {
    #pragma HLS PIPELINE II=1
    float sum = 0.0f;
    for (int k = 0; k < N; k++)
        sum += a_local[k] * B[k*N+j];
    C[i*N+j] = sum;
}
```

Each `j` iteration gets an independent `sum` — no shared state, no mux.
**II=1, Fmax=136MHz.** Best non-tiled result, but B is still strided in
DRAM since this version doesn't address memory layout, only the HLS
scheduling problem.

---

## 4. Adding Tiling + Hardware Transpose

To cut DDR traffic from N³ to N³/T and fix the strided-B problem at the
source, two things were added:

1. **`transpose()`** — a separate HLS function that reads B row-by-row via
   ACP and writes `B_T` (transposed) to a DDR scratch buffer via HP0. After
   this, `B_T[j][k] == B[k][j]`, so reading "row j of B_T" is the same data
   as "column j of B," but now sequential in memory.
2. **16×16 tiles** (`a_tile`, `b_tile`, `c_tile`) held in BRAM. Each tile is
   loaded once from DDR and reused `TILE` times before eviction, cutting
   DDR reads from N³ down to roughly `2·N³/TILE`.

### Tiling attempt 1 — UNROLL `k` only

16 chained `fadd` operations in series → **II=64, Fmax=69MHz**. The
adder chain became the critical path.

### Tiling attempt 2 — explicit balanced adder tree (depth 4, not 16)

Rewrote the accumulation as a manual reduction tree
(16 products → 8 → 4 → 2 → 1). Fixed the chain, but
`c_tile[i][j] += tree_result` still read-modified-wrote the same BRAM
cell across `j` iterations, creating a mux. **II=4, Fmax=107MHz.**

### Tiling attempt 3 — UNROLL `j`, explicit adder tree

Moved `PIPELINE` to the `i` loop and fully unrolled `j`. Each `j` now has
its own independent `c_tile[i][j]` register — no shared state.
**II=1, Fmax=137MHz.** 256 `fmul` + 256 `fadd` instances generated.

### Tiling attempt 4 — UNROLL `j`, simple `sum +=` (no manual tree)

Tested whether the explicit tree was actually necessary by unrolling
both `j` and `k` and just writing `sum += a_tile[i][k] * b_tile[k][j]`
directly. HLS auto-inferred an equivalent reduction tree.
**II=1, Fmax=137MHz** — functionally identical to attempt 3, simpler
code, slightly more `fadd` instances (272) and deeper pipeline (92 vs
32 cycles), but well within tolerance.

**This is the current kernel structure** — tiled, transposed-B, fully
unrolled `j`/`k` compute, II=1 on every loop, all loop constraints
satisfied.

---

## 5. DSP Resource Problem (current blocker)

With `TILE=16`, the fully-unrolled compute stage instantiates 256
parallel multiply-accumulate units:

```
256 fmul × ~3 DSPs + 272 fadd × ~2 DSPs ≈ 1312 DSP48E1 required
XC7Z020 only has 220 DSP48E1 sites available
```

`place_design` fails with `DRC UTLZ-1: DSP48E1 over-utilized`.

**Fix in progress:** reduce `TILE` from 16 to 4. This drops the unrolled
MAC count to 16, bringing DSP usage to roughly 80 — comfortably within
the 220 budget — while still preserving most of the tiling benefit
(DDR reads drop by `TILE`×, just 4× instead of 16×).

---

## 6. Vivado Integration Notes

A few non-obvious things learned while wiring this into the block design:

- **`B_T` is not a separate AXI port.** It shares the `ACP` bundle with
  `A` and `B` in the HLS pragma, so HLS merges them into a single
  `m_axi_ACP` master. The original 3-port interface (`m_axi_ACP`,
  `m_axi_HP0`, `s_axi_control`) is correct — no extra port to connect.
- **IP cache staleness.** Vivado can keep serving an old cached
  `system_matmul_0_0` IP definition from `project_2.gen/.../ip/` even
  after `update_ip_catalog -rebuild` and "Upgrade IP." If the new ports/
  registers don't show up after an upgrade, delete the cached IP folder
  and recreate the cell from scratch:
  ```tcl
  delete_bd_objs [get_bd_cells matmul_0]
  update_ip_catalog -rebuild -repo_path <path-to-matmul_hls>
  create_bd_cell -type ip -vlnv xilinx.com:hls:matmul:1.0 matmul_0
  ```
- **Clock frequency mismatch.** The kernel was synthesized targeting
  100MHz but the block design clock (`FCLK_CLK0`) is 50MHz. Set
  `CONFIG.FREQ_HZ 50000000` explicitly on `m_axi_ACP`, `m_axi_HP0`,
  `s_axi_control`, and `ap_clk` after adding the IP, or `validate_bd_design`
  throws `FREQ_HZ` mismatch errors.
- Both ACP and HP0 paths from `matmul_0` go through dedicated 2-input
  `axi_interconnect` blocks (`axi_intercon_acp`, `axi_intercon_hp0`)
  shared with the DMA's existing masters, since two masters can't drive
  one slave port directly.

---

## 7. Current Status

```
✓ figures2  — ACP vs HP0 vs SW baseline, clean results, analyzed
✓ figures3  — first HW matmul integrated; correctly identified as 10×
              slower than SW due to strided B access (useful negative
              result, root cause diagnosed)
✓ Tiled + transposed kernel written and synthesized — II=1 on every
  loop, Fmax≈137MHz, all loop constraints satisfied
✗ TILE=16 bitstream fails DSP placement (1312 needed vs 220 available)
□ Pending: resynthesize with TILE=4, confirm DSP count fits in HLS
  synthesis report before regenerating the bitstream
□ Pending: run C Simulation in Vitis HLS against a software reference
  (identity-matrix test) to verify kernel correctness before re-export —
  not yet done, recommended before burning another bitstream cycle
□ Pending: re-export IP → refresh Vivado IP catalog → reconnect
  matmul_0 → regenerate bitstream
□ Pending: hardware correctness check (4×4 identity matrix) on the
  actual board before running the full sweep
□ Pending: figures4 — full N=32..512 sweep with the corrected/tiled
  accelerator (ACP, HP0, SW, MATMUL all in one comparison)
□ Pending: final report incorporating all four benchmark rounds
```

**Immediate next step:** change `#define TILE 16` to `#define TILE 4` in
`matmul.cpp`, run C Simulation to check correctness, then re-run C
Synthesis and confirm DSP usage is under 220 before touching Vivado.

---

## 8. File Locations

```
matmul_hls/matmul.cpp     HLS source (transpose + tiled matmul kernel)
matmul_hls/export.zip     Exported Vivado IP (regenerate after each
                           HLS change — re-export, don't reuse a stale zip)
project_2/                Vivado project (block design: system.bd)
```