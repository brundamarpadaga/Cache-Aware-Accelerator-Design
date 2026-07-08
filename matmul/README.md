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

### Pragma experiments — accumulation loop (on `a_local`, v1 → v3)

Getting `sum += a_local[k] * B[k*N+j]` to pipeline at II=1 took several
attempts, each fighting a different flavor of loop-carried dependency
on `sum`:

| # | Approach | Result |
|---|---|---|
| 1 | Plain `#pragma HLS PIPELINE` on `k` loop | Baseline attempt, no improvement |
| 2 | `PIPELINE II=1` + `DEPENDENCE ... intra false` on `sum` | No effect — dependency is real (each iteration needs the prior accumulated value); `DEPENDENCE` also can't apply to a scalar |
| 3 | `PIPELINE II=1` + `UNROLL factor=5` | **II=25** — 5-wide chain of 5-cycle fadds serializes to 5×5 |
| 4 | Manual `acc[5]` array, `k % 5` indexing | **II=4** — runtime index creates a mux in front of the adder |
| 5 | Manual `acc[5]` array, explicit wrap-around counter | **II=4** — same mux problem; index is still a runtime value regardless of how it's computed |
| **6 (fix)** | **`PIPELINE` moved to `j` loop**, `k` left to auto-unroll underneath | **II=1, Fmax=136MHz ✓** — each `j` iteration gets its own independent `sum`, nothing left to multiplex |

Attempt 6 (v3) was the version first exported to hardware
(`figures3`). It ran **10× slower than software matmul** at N=512
(~1s vs 97ms) — II=1 fixed the on-chip scheduling, but `B[k*N+j]` was
still column-strided in DDR, so N³ individual non-burst reads
dominated runtime regardless of pipeline cleanliness. This is the
baseline the tiling work in Section 4 fixed at the memory-layout level.

---

## 4. HW v2 — Tiling, Hardware Transpose, and Correctness Fixes

### Motivation

With `a_local` (Section 3) handling A's reuse, B was still read
column-wise straight from DDR on every multiply — no burst, no reuse.
Two changes were needed to fix this at the source:

| Change | Purpose |
|---|---|
| **Hardware `transpose()`** | Converts B's column access into row access: writes `B_T` to a DDR scratch buffer such that `B_T[j][k] == B[k][j]`, so "row j of B_T" is the same data as "column j of B," but sequential in memory. |
| **16×16 tiling** (`a_tile`, `b_tile`, `c_tile` in BRAM) | Each tile loaded once from DDR, reused `TILE` times before eviction — cuts DDR reads from N³ toward `2·N³/TILE`. |

### Pragma experiments — tiled compute loop (c_tile accumulation)

| # | Approach | Result |
|---|---|---|
| 1 | `UNROLL k` only | **II=64, Fmax=69MHz** — 16-deep adder chain becomes the critical path |
| 2 | Explicit balanced adder tree (depth 4, not 16) | **II=4, Fmax=107MHz** — chain fixed, but `c_tile[i][j] += tree_result` still read-modify-writes the same cell across `j`, creating a mux |
| 3 | `PIPELINE` on `i`, `UNROLL j`, explicit adder tree | **II=1, Fmax=137MHz ✓** — each `j` gets its own `c_tile[i][j]` register, no shared state |
| **4 (final)** | Same as 3 but simple `sum +=` (no manual tree) | **II=1, Fmax=137MHz ✓** — HLS auto-infers an equivalent tree; simpler code, functionally identical |

Attempt 4 became the shipped `TILE`-based kernel structure.

### DSP budget forced `TILE` down from 16 to 4

At `TILE=16`, the fully-unrolled compute stage needed ~1312 DSPs
(`256 fmul × ~3 + 272 fadd × ~2`) against the XC7Z020's 220 available
— `place_design` failed DRC `UTLZ-1`. Reduced `TILE` to 4 (~97 DSPs,
comfortably under budget), trading some tiling benefit (DDR reads drop
`TILE×` instead of 16×) for a design that actually places.

### Bugs found and fixed during HW v2 development

| Bug | Symptom | Root cause | Fix / status |
|---|---|---|---|
| **`b_tile` indexing swap** | Wrong on 75% of outputs; identity-matrix testing nearly masked it entirely | Compute used `b_tile[k][j]` instead of the load convention `b_tile[j][k]` | Swapped indices; verified via patterned, non-symmetric testbench (`matmul_tb_pattern.cpp`) — this is *why* that testbench exists |
| **AXI write/read race on `B_T`** | Non-deterministic NaN/zero on hardware — *different* garbage each identical re-run | `A`/`B`/`B_T` share one `m_axi_ACP` bundle; `transpose`'s `ap_done` only guarantees writes were queued (`num_write_outstanding=16`), not drained through the AXI4→AXI3 converter before `matmul_tiled` starts reading `B_T` | Fix designed (dedicated `B_T` bundle, `num_write_outstanding=1`) — **still not applied**, largest open correctness risk in the project (see Section 5) |
| **Transpose non-burst write** | `B_T[j*N+i] = B[i*N+j]` bursts on read, single-beat on write (strided by N) | — | Tile-buffered load/store (`T_TILE=16`): buffer a 16×16 block in BRAM, write it back with the transposed loop order so *both* sides burst. Confirmed at synthesis: both loops infer 16-beat bursts, II=1, ~2 BRAM blocks, ~11 DSPs (address arithmetic only) |
| **cosim: `m_axi` `depth` unspecified** | Silent `SIGSEGV` (`0xC0000005`) in `transpose`'s store loop, no test output printed | With `N` runtime-variable, cosim's C-TB memory model defaults far too small (buffers landed ~128 floats apart vs. the ~1024 needed) — simulation-only artifact, no effect on real hardware | `depth=MAX_N*MAX_N` added to all four `m_axi` ports |
| **cosim: testbench arrays sized `N*N`** | Crash moved to `copy_in`/`onebyonecpy_hls...262144` | `depth` fix told cosim to copy `MAX_N*MAX_N` elements from testbench buffers only sized `N*N` | Testbench arrays resized to `MAX_N*MAX_N` (both `matmul_tb.cpp` and `matmul_tb_pattern.cpp`) |

*(Diagnosed via `gdb` directly on the generated `cosim.tv.exe` — `run` +
`bt` gave an exact crash line/backtrace each time, faster than
guessing from the silent-failure symptom alone.)*

### HW v2 result

- **Cosim: PASS**, both testbenches, cycle-accurate RTL match against
  the C model. N=32 patterned test: **69,142 cycles**
  (≈1.38ms @ actual 50MHz deployment clock).
- **Hardware N=4 correctness check: PASS** — treated as *promising,
  not conclusive* re: the AXI race, since tiling shrinks the race
  window (fewer, larger write bursts) without closing it, and a single
  small-N pass is weak evidence for a probabilistic race.
- **Performance bottleneck identified:** `TILE=4` at N=512 measured
  **~7.11s** on hardware. Root cause: every `tk` step fetches a 4×4
  tile (16 elements) to do 16 MACs — one DDR round trip per useful
  multiply, load and compute run strictly sequentially (no
  double-buffering to hide the round-trip latency). This motivated
  HW v3 below.

---

## 5. HW v3 — `FETCH_TILE=8` DDR Round-Trip Optimization

### Motivation

Decouple "how much data is fetched per DDR round-trip" from "how many
DSPs the compute engine uses" — fetch a bigger block per trip, but
keep consuming it with the same small, DSP-cheap `TILE=4` engine.

### `TILE=8` tried first — DSP-infeasible

Naive assumption: DSPs scale ~linearly with `TILE`. Wrong — both `j`
and `k` are fully unrolled, so cost scales as `TILE²`:

| TILE | Unrolled MACs (TILE²) | Actual DSPs | DSPs per MAC |
|---|---|---|---|
| 4 | 16 | 97 | ≈5.5× |
| 8 | 64 | 336 | ≈5.25× |
| 16 | 256 | 1312 | ≈5.1× |

`TILE=8` needs 336 DSPs against 220 available — `place_design` failed
DRC `UTLZ-1`. No power-of-2 `TILE` between 4 and 8 fits. Rejected in
favor of decoupling fetch size from compute size instead.

### `FETCH_TILE=8` design

Keep the `TILE=4` engine unchanged (DSP budget untouched); fetch an
8×8 block per DDR round-trip and loop over four `TILE=4` sub-blocks to
consume it:

```cpp
#define TILE        4    // compute sub-tile — DSP budget, unchanged
#define FETCH_TILE  8    // DDR fetch tile — data per round-trip
                          // FETCH_TILE must be a multiple of TILE
                          // N must be a multiple of FETCH_TILE

for (int si = 0; si < FETCH_TILE; si += TILE) {
  for (int sj = 0; sj < FETCH_TILE; sj += TILE) {
    for (int sk = 0; sk < FETCH_TILE; sk += TILE) {
      #pragma HLS LOOP_FLATTEN off      // see bug below
      for (int i = 0; i < TILE; i++) {
        #pragma HLS PIPELINE II=1
        for (int j = 0; j < TILE; j++) {
          #pragma HLS UNROLL
          float sum = 0.0f;
          for (int k = 0; k < TILE; k++) {
            #pragma HLS UNROLL
            sum += a_tile[si+i][sk+k] * b_tile[sj+j][sk+k];
          }
          c_tile[si+i][sj+j] += sum;
        }
      }
    }
  }
}
```

Cuts `tk`-iterations from `N/TILE` to `N/FETCH_TILE` — at N=512, 128 →
64 per output tile, ≈8× fewer total round-trips
(`(FETCH_TILE/TILE)³`), with DSP count unchanged.

### Bugs found and fixed during HW v3 development

| Bug | Symptom | Root cause | Fix |
|---|---|---|---|
| **II=2 violation** | `"Unable to enforce a carried dependence constraint (distance=4)"` on `c_tile`; loop constraints NOT satisfied | `sk`'s body is a single clean child loop (`i`) — HLS auto-flattened it into the II=1 pipeline with no warning, unlike `tk` (which has 3 sibling sub-loops and is structurally protected from this). Consecutive `sk` values got treated as one continuous stream, so a `c_tile` write from `sk=0` could still be in flight when `sk=4` tried to read the same address | `#pragma HLS LOOP_FLATTEN off` on `sk` — forces each `(si,sj,sk)` combination to drain before the next starts. Re-synthesis confirmed **II=1 restored**, DSPs back to the clean `97`-ish baseline (`16 fmul = TILE²`), bursts still length-8, Fmax unchanged |
| **`matmul_tb.cpp` cosim compile error** | `'C_ref' was not declared in this scope` | Dropped during the earlier `MAX_N*MAX_N` testbench-resizing edit (Section 4), for this file specifically | Restored the declaration |
| **GUI showed `Fail` after console `PASS`** | Cosim Report panel displayed `Fail` right after a run whose console log clearly ended `TEST PASSED` / `*** PASS ***` | Stale cached GUI view from an earlier, genuinely-failed run of the same testbench; panel didn't auto-refresh | Confirmed via direct file read + `LastWriteTime` on `matmul_cosim.rpt` — file matched the passing run. **Lesson: trust the report file over the GUI panel when they disagree.** |
| **N=4 correctness check FAIL** | `got = 2×expected + 5` for the first 11 outputs (clean affine pattern, not race-style noise), then uninitialized-looking garbage for the rest | `N=4 < FETCH_TILE=8` — inner load/compute loops always run their full 8×8 extent regardless of `N`, reading past the logical 4×4 matrix into adjacent memory | Correctness check moved from N=4 to **N=32** (smallest size that's a multiple of `FETCH_TILE`/`T_TILE`=16); documented as a hard kernel constraint rather than special-cased |
| **`figures5` README overstated HW v3 latency 33–40%** | Summary's HW v3 column didn't match the raw UART sweep log at any N, gap widening with N; ACP/HP/HW v2 columns matched fine | Summary appears to have been built from a stale or mismatched log file (header referenced a `hw4` log in a `v2`/`v3`-labeled folder) | Recalculated all HW v3 figures directly from the sweep log's `MATMUL,N,elapsed_us` rows |

### HW v3 result

Cosim: **PASS**, both testbenches, N=32 patterned test now **42,830
cycles** (down from 69,142 pre-`FETCH_TILE=8` — ≈38% cycle reduction
at the RTL level). Hardware: clean at every tested size once the N=4
constraint was corrected to N=32.

**Corrected real-hardware performance** (see
`figures5-hwAccelerator3-tiling-v2/README` for the full sweep,
SW comparison, and cache-behavior figures):

| N | HW v2 (µs) | HW v3 (µs) | Speedup (v2/v3) |
|---|---|---|---|
| 32 | 1,985 | 1,194 | 1.66× |
| 64 | 14,776 | 8,545 | 1.73× |
| 128 | 114,070 | 64,588 | 1.77× |
| 256 | 896,958 | 502,071 | 1.79× |
| 512 | 7,113,180 | 3,962,935 | **1.80×** |

At N=512: **~7.11s → ~3.96s**, close to the ~3.9s figure derived
analytically from per-loop synthesis latencies — real hardware landing
near the ideal-latency estimate supports the `FETCH_TILE=8`
round-trip-reduction model being correct, not just plausible-sounding.
HW v3 vs. SW matmul speedup ranges 2.0–2.5× across N=32–512.

**None of the above touches the Section 4 AXI race.** All HW v3
testing validates `FETCH_TILE=8` correctness and performance
specifically; it is not evidence for or against the race one way or
the other.

---

## 6. Vivado Integration Notes

A few non-obvious things learned while wiring this into the block design:

- **`B_T` is not a separate AXI port.** It shares the `ACP` bundle with
  `A` and `B` in the HLS pragma, so HLS merges them into a single
  `m_axi_ACP` master. The original 3-port interface (`m_axi_ACP`,
  `m_axi_HP0`, `s_axi_control`) is correct — no extra port to connect.
  (This shared bundle is also the root cause of the still-open AXI
  race — see Section 4.)
- **IP cache staleness.** Vivado can keep serving an old cached
  `system_matmul_0_0` IP definition from `project_2.gen/.../ip/` even
  after `update_ip_catalog -rebuild` and "Upgrade IP." If new ports/
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
  `s_axi_control`, and `ap_clk` after adding the IP, or
  `validate_bd_design` throws `FREQ_HZ` mismatch errors.
- Both ACP and HP0 paths from `matmul_0` go through dedicated 2-input
  `axi_interconnect` blocks (`axi_intercon_acp`, `axi_intercon_hp0`)
  shared with the DMA's existing masters, since two masters can't drive
  one slave port directly.

---

## 7. Current Status

```
✓ figures2  — ACP vs HP0 vs SW baseline, clean results, analyzed
✓ figures3  — first HW matmul integrated; correctly identified as 10×
              slower than SW due to strided B access (root cause
              diagnosed, fixed via tiling in HW v2)
✓ HW v2 (TILE=4) — tiled + transposed kernel, b_tile bug fixed, cosim
  PASS (69,142 cycles @ N=32), hardware N=4 PASS (promising, not
  conclusive re: the AXI race), ~7.11s @ N=512 measured
✓ HW v3 (FETCH_TILE=8) — sub-tiled kernel, II=1 restored after the
  LOOP_FLATTEN fix, DSPs unchanged (~97), cosim PASS on both
  testbenches (42,830 cycles @ N=32), hardware clean at N=32..512
✓ N=4 correctness-check convention retired (N < FETCH_TILE is an
  unsupported boundary case) — moved to N=32
✓ Real ~44% latency reduction at N=512 confirmed on hardware
  (7.11s → 3.96s), consistent with analytical prediction
✓ figures5 benchmark README corrected after a stale-log discrepancy
  (33–40% HW v3 overstatement at every N in the first draft)
□ Pending: apply the AXI-race bundle-split fix (dedicated B_T bundle,
  num_write_outstanding=1) and re-test at larger N with many repeated
  runs before treating the race as resolved — still the single
  largest open correctness risk in the project
□ Pending: final report incorporating all five benchmark rounds
```

---

## 8. File Locations

```
matmul_hls/matmul.cpp             HLS source (transpose + tiled matmul kernel)
matmul_hls/matmul_tb.cpp          C sim / cosim testbench: identity A, N=16
matmul_hls/matmul_tb_pattern.cpp  C sim / cosim testbench: patterned A, N=32
matmul_hls/export.zip             Exported Vivado IP (regenerate after each
                                    HLS change — re-export, don't reuse a stale zip)
project_2/                        Vivado project (block design: system.bd)
analysis/figures5-hwAccelerator3-tiling-v2/README
                                   HW v3 benchmark results, corrected numbers
```