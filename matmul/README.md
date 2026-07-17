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

## 6. HW v4 — `FETCH_TILE=16` and Dedicated `B_T` AXI Bundle

### Motivation

Two independent threads: keep pushing the DDR round-trip reduction from
Section 5, and finally apply the AXI-race fix designed back in Section 4
but never implemented — `A`/`B`/`B_T` still shared one `m_axi_ACP`
bundle, still the largest open correctness risk in the project.

### Resource exploration — fabric-implemented float multiply

Tried freeing DSP budget via `#pragma HLS BIND_OP variable=prod op=fmul
impl=fabric` (leaving `fadd` DSP-backed), to see if it would make
`TILE=8`'s fully-unrolled compute engine finally fit:

| Config | DSP | LUT | Verdict |
|---|---|---|---|
| `TILE=4`, `fmul`→DSP (baseline) | 97 (44%) | 41% | — |
| `TILE=4`, `fmul`→fabric | 51 (23%) | 66% | Fits, II still 1, cosim PASS |
| `TILE=8`, `fmul`→fabric | 155 (70%) | **100,367 (188%)** | **Infeasible — LUTs, not DSPs** |

`TILE=8` DSP cost was never the real wall once `fmul` was fabric-backed
(DSPs comfortable at 70%) — but quadrupling fabric-multiply instances
(`TILE²`) blew LUTs to nearly double the device's capacity instead.
Confirms DSP-vs-LUT is a real tradeoff, not a free win, and `TILE=4`
remains the practical ceiling for the fully-unrolled engine.

**`fmul`→fabric was reverted before the hardware test below**, to keep
the `B_T` bundle-split fix isolated to one variable rather than testing
it simultaneously with an unrelated datapath change — the two don't
interact, but a clean attribution mattered more than saving one
synthesis cycle.

### `FETCH_TILE` 8 → 16

Same mechanism as Section 5's 4→8 change, one more doubling:

| FETCH_TILE | Burst length | Cosim cycles (N=32) |
|---|---|---|
| 8 | 8 | 42,830 |
| 16 | 16 | 32,534 (DSP-mult) → **29,817** (final, with `B_T` split below) |

`(32/16)²` fewer tiles, `N/16` fewer `tk`-steps per tile — same pattern
as before, diminishing but still real returns. `FETCH_TILE=32` was
considered but not pursued this round (BRAM cost doubles again, and the
returns were already narrowing).

### Dedicated `B_T` AXI bundle — the race fix, finally applied

```cpp
#pragma HLS INTERFACE m_axi port=B_T bundle=BT offset=slave depth=512*512 \
    num_read_outstanding=1  max_read_burst_length=16 \
    num_write_outstanding=1 max_write_burst_length=16
```
(`A`, `B`, `C` unchanged from Section 4/5.) `B_T` gets its own `m_axi`
master port with outstanding capped at 1 — the AXI adapter cannot
issue a new command on this port until the previous one's response
(BRESP/RLAST) is received, so `transpose`'s write to `B_T` must fully
drain before `matmul_tiled`'s first `B_T` read can even be dispatched.
This is the actual ordering guarantee; it sits at the adapter's
command-queue level, independent of which physical interconnect the
port is later wired through.

**Confirmed at synthesis:** new `m_axi_BT` interface, burst length
still 16 (outstanding=1 limits transactions *in flight*, not *burst
size*), correctly split from `A`/`B`'s `m_axi_ACP`.

### Vivado integration — recreating `matmul_0` drops *all* existing wiring, not just the new port

Adding a third AXI master forced deleting and recreating the `matmul_0`
IP cell (Vivado's automatic IP *upgrade* path failed with an
unresolvable `CoreID 2-1279` warning on the new port — delete-and-recreate
is the reliable path, matching the project's existing IP-cache-staleness
lesson from Section 7/Vivado notes). The costly discovery: **recreating
the cell silently drops every pre-existing connection**, not just the
new one — `m_axi_ACP`, `s_axi_control`, `ap_clk`/`ap_rst_n` all need
reconnecting, and several came back as *stale nets* (a net with the old
name still existed on the interconnect side, but wasn't actually
attached to the new cell's pin) — invisible in the block-design canvas,
only caught by querying each pin directly from both ends:

| Symptom | Root cause | Fix |
|---|---|---|
| `S01_AXI` showed net `/matmul_0_m_axi_ACP`, but `get_bd_intf_nets` on `matmul_0/m_axi_ACP` returned empty | Stale net name left from the deleted cell; new cell's pin was never actually attached | `connect_bd_intf_net` explicitly, re-verify from both ends |
| Same pattern on `s_axi_control` → `ps7_0_axi_periph/M01_AXI` | Same — stale net, unattached pin | Same fix |
| `FREQ_HZ` warnings on all four interfaces after recreation | New cell has no `CONFIG.FREQ_HZ`; doesn't propagate automatically from the clock net | `set_property CONFIG.FREQ_HZ 50000000` on each interface explicitly |
| `HP0` address unassigned (`No address segments matched ... axi_intercon_hp0/S01_AXI`) | New cell → new/cleared address assignment | `assign_bd_address` (broad, re-run for whole design) |

**Lesson for next time:** after any IP cell delete/recreate, verify
*every* pin's connection from the cell's own side
(`get_bd_intf_nets -of_objects [get_bd_intf_pins matmul_0/<pin>]`), not
from the interconnect's side and not by trusting the canvas rendering
— three separate "already connected" assumptions turned out to be
stale nets this session, caught only by checking both ends.

### Hardware validation — `figures6`, corrected

```
  N     HW v3 (µs)   HW v4 (µs)   Speedup      HW v4 vs SW
  ---   ----------   ----------   ---------    -----------
   32        1,194          686   1.74×        3.53×
   64        8,545        4,612   1.85×        4.15×
  128       64,588       33,552   1.93×        4.55×
  256      502,071      255,167   1.97×        4.78×
  512    3,962,935    1,993,172   1.99×         4.89×
```
Speedup *increases* with N (opposite of the expected serialization
penalty from `num_write_outstanding=1`) — suggests the dedicated
bundle's reduced contention with `A`/`B` traffic outweighs the
outstanding=1 cost, though `FETCH_TILE=16` and the bundle split
shipped together here, so exact attribution between the two isn't
possible from this data alone (flagged in the `figures6` doc itself).

### Correctness-evidence gap found in the benchmark harness itself

Reviewing `benchmark.c`/`main.c` directly (not just log output) found
the sweep that produces the `figures6` timing table has **no
correctness checking in any of its 25 runs** — `benchmark_matmul()`
times `ap_done` and invalidates cache, nothing else; `print_csv_row()`
has no pass/fail field. The only correctness check is a single,
one-time, identity-matrix gate at `N=16` in `main()` before the sweep
starts (halts on failure — good — but doesn't cover N=32..512, and
identity-`A` doesn't exercise the general accumulation path the way a
non-symmetric pattern would, same blind spot as the original
`b_tile` bug).

**Follow-up cross-check** (asymmetric `A`/`B`, `N=32`, bit-exact `!=`
comparison against a software reference) reported 87 mismatches —
**decoded as 2 ULP** on every flagged value (e.g.
`0x4BA1A958` vs `0x4BA1A956`), i.e. floating-point summation-order
rounding noise (tiled `si`/`sj`/`sk` accumulation vs. linear software
accumulation), not corruption. Same root issue as the tolerance-based
comparison already used in `matmul_tb_pattern.cpp`
(`fabsf(diff) > tol`) — the bare-metal cross-check needs the same
tolerance logic; bit-exact `!=` will always flag this class of
harmless rounding difference as a failure.

**Net effect on the race question:** unchanged from Section 4/5 —
still not resolved either way. `figures6`'s repeated hardware runs
prove the bundle-split design doesn't hang/crash across N=32–512, but
provide no correctness evidence at those sizes (sweep has none), and
the one cross-check that did compare values used the wrong comparison
method to be conclusive.

### Updated status checklist (supersedes Section 5/13's)

```
✓ FETCH_TILE 8→16, confirmed at synthesis and cosim (29,817 cycles @ N=32,
  down from 42,830 at FETCH_TILE=8)
✓ TILE=8 + fabric-mult explored, correctly diagnosed as LUT-infeasible
  (188% utilization) — DSP was never actually the wall once mult was
  fabric-backed; LUTs are
✓ B_T moved to a dedicated m_axi bundle, num_outstanding=1 — the AXI
  race fix from Section 4, now actually applied (not just designed)
✓ Full Vivado re-wiring completed after matmul_0 cell recreation,
  including catching 3 stale/unattached-pin cases via direct
  per-pin verification
✓ Hardware validated across N=32..512, real ~2× speedup over HW v3,
  ~4.9× over SW at N=512 (figures6, cross-checked against raw log —
  no stale-data repeat of the figures5 incident)
✗ Gap found: benchmark sweep has zero correctness checking at the
  sizes it actually reports timing for (N=32..512) — only a single
  N=16 identity-matrix gate check runs, before the sweep
✗ Cross-check test built to close that gap used bit-exact comparison;
  flagged 87 "mismatches" that decode to 2 ULP floating-point rounding
  noise, not real errors — needs the same tolerance-based comparison
  already used in matmul_tb_pattern.cpp
□ Pending: add tolerance-based correctness checking inside (or
  alongside) the benchmark sweep itself, across all tested N, not just
  a single N=16 gate — this is the evidence that would actually confirm
  or refute the race fix, and it still doesn't exist
□ Pending: once real per-N correctness data exists, get many repeated
  runs at larger N (64/128+) specifically — still the strongest
  remaining test of whether the race is genuinely fixed
□ Pending: final report incorporating all six benchmark rounds
```

---

## 7. Vivado Integration Notes

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

## 8. Current Status (superseded — see Section 6's checklist for the latest)

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
  unsupported boundary case) — moved to N=32, later to N=16
✓ Real ~44% latency reduction at N=512 confirmed on hardware
  (7.11s → 3.96s), consistent with analytical prediction
✓ figures5 benchmark README corrected after a stale-log discrepancy
  (33–40% HW v3 overstatement at every N in the first draft)
```

This checklist is out of date as of HW v4 (Section 6) — the AXI-race
bundle-split fix below has since been designed *and applied*, but real
correctness evidence at the benchmarked sizes still doesn't exist (see
Section 6's own checklist for the accurate, current state).

---

## 9. File Locations

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