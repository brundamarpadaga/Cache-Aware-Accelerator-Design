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

## 3b. Pragma Experiments on the Accumulation Loop

With `a_local` in place (v1), the next problem was the inner
accumulation loop (`sum += a_local[k] * B[k*N+j]`) — getting it to
pipeline at II=1 took several attempts, each fighting a different
flavor of loop-carried dependency on `sum`.

### Attempt 1 — Plain `#pragma HLS PIPELINE`

```cpp
for (int k = 0; k < N; k++) {
    #pragma HLS PIPELINE
    sum += a_local[k] * B[k * N + j];
}
```

The simplest thing to try first — pipeline the accumulation loop
directly with no other hints.

### Attempt 2 — `PIPELINE II=1` + `DEPENDENCE` override

```cpp
for (int k = 0; k < N; k++) {
    #pragma HLS PIPELINE II=1
    #pragma HLS DEPENDENCE variable=sum type=intra false
    sum += a_local[k] * B[k * N + j];
}
```

Tried telling HLS to ignore the intra-loop dependency on `sum` via
`DEPENDENCE`. This pragma is meant for *false* dependencies the tool
over-conservatively assumes — but the dependency on `sum` here is real
(each iteration genuinely needs the previous iteration's accumulated
value), so overriding it doesn't change the scheduling problem.
`DEPENDENCE` also cannot be applied to a scalar variable, so this
pragma had no effect at all.

### Attempt 3 — `PIPELINE II=1` + `UNROLL factor=5`

```cpp
for (int k = 0; k < N; k++) {
    #pragma HLS PIPELINE II=1
    #pragma HLS UNROLL factor=5
    sum += a_local[k] * B[k * N + j];
}
```

Partially unrolling by 5 to give the adder more independent work per
cycle. This produced warnings and left `sum` as a single shared
accumulator across the 5 unrolled copies, so the dependency chain was
made 5-wide instead of removed. Result: **II=25** — since each
floating-point add takes 5 cycles, the 5-wide chain serializes to
5×5=25.

### Attempt 4 — Manual 5-element accumulator array, modulo indexing

```cpp
for (int j = 0; j < N; j++) {
    float acc[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    #pragma HLS ARRAY_PARTITION variable=acc complete

    for (int k = 0; k < N; k++) {
        #pragma HLS PIPELINE II=1
        acc[k % 5] += a_local[k] * B[k * N + j];
    }

    C[i * N + j] = acc[0] + acc[1] + acc[2] + acc[3] + acc[4];
}
```

The idea: spread the accumulation across 5 independent registers
(`acc[0..4]`) using `k % 5` to round-robin between them, breaking the
single-accumulator chain, then sum the 5 partial results at the end —
effectively a manual version of what `UNROLL` was trying to do
automatically. The modulo only added complexity without helping:
**II=4**, since `acc[k % 5]` still uses a runtime index and HLS
synthesizes a mux to route the write.

### Attempt 5 — Same idea, explicit counter instead of `%`

```cpp
for (int j = 0; j < N; j++) {
    float acc[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    #pragma HLS ARRAY_PARTITION variable=acc complete dim=1

    int acc_idx = 0;

    for (int k = 0; k < N; k++) {
        #pragma HLS PIPELINE II=1
        acc[acc_idx] += a_local[k] * B[k * N + j];
        acc_idx = (acc_idx == 4) ? 0 : acc_idx + 1;
    }

    C[i * N + j] = acc[0] + acc[1] + acc[2] + acc[3] + acc[4];
}
```

Swapped `%` for an explicit increment-and-wrap counter, in case the
modulo operation itself was blocking compile-time index resolution.
Same outcome as Attempt 4: still **did not reach II=1**, because
`acc_idx` is a runtime value regardless of how it's computed — HLS
cannot determine at compile time which `acc[]` element a given
iteration writes to, so it still synthesizes a mux to route the write.
That mux sits in series with the floating-point adder on the critical
path, which is what capped the achievable II.

### The real fix — pipeline the `j` loop, not the `k` loop

```cpp
for (int i = 0; i < N; i++) {

    for (int k = 0; k < N; k++) {
        #pragma HLS PIPELINE II=1
        a_local[k] = A[i * N + k];
    }

    for (int j = 0; j < N; j++) {
        #pragma HLS PIPELINE II=1
        float sum = 0.0f;

        for (int k = 0; k < N; k++) {
            sum += a_local[k] * B[k * N + j];
        }

        C[i * N + j] = sum;
    }
}
```

Instead of trying to break the dependency *within* one `j` iteration's
accumulation, this sidesteps the problem entirely: `PIPELINE` moves to
the `j` loop, and `k` is left to auto-unroll underneath it. Each `j`
iteration now gets its own independent `sum` — no register is shared
*across* `j` iterations, so there's nothing left for HLS to multiplex.
**This achieved II=1, Fmax=136MHz** — the best non-tiled result, and
the version that was exported and integrated into hardware first
(`figures3`). On the board it ran **10× slower than software matmul**
at N=512 (~1s vs 97ms): II=1 fixed the HLS scheduling problem, but
`B[k*N+j]` is still column-strided in DDR — addresses jump by N×4
bytes every read, so AXI cannot burst these transfers, and N³
individual non-burst reads dominate the runtime regardless of how
cleanly the loop pipelines on-chip. This became the baseline (v3) the
tiling work in Section 4 built on, this time fixing the memory-layout
problem at its source via tiling and a hardware transpose of B.

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

## 7. Current Status (superseded — see Section 13)

```
✓ figures2  — ACP vs HP0 vs SW baseline, clean results, analyzed
✓ figures3  — first HW matmul integrated; correctly identified as 10×
              slower than SW due to strided B access (useful negative
              result, root cause diagnosed)
✓ Tiled + transposed kernel written and synthesized — II=1 on every
  loop, Fmax≈137MHz, all loop constraints satisfied
✗ TILE=16 bitstream fails DSP placement (1312 needed vs 220 available)
✓ TILE=4: resynthesized with TILE=4, confirm DSP count fits in HLS
  synthesis report before regenerating the bitstream
✓ C Simulation run against software reference — caught a real
  correctness bug (Section 8), fixed, re-verified passing
✓ re-export IP → refresh Vivado IP catalog → reconnect
  matmul_0 → regenerate bitstream
✓ hardware correctness check (N=4) — see Sections 9–12 for the full
  non-determinism investigation this uncovered
□ Pending: figures4 — full N=32..512 sweep with the corrected/tiled
  accelerator (ACP, HP0, SW, MATMUL all in one comparison)
□ Pending: final report incorporating all four benchmark rounds
```

This checklist is out of date the moment `TILE=4` hardware testing
began — see Section 13 for the current, accurate status. Left here
unedited as a record of what was known at the time.

---

## 8. Critical Bug — `b_tile` Indexing Swap

C Simulation (run before touching hardware, per the note at the end of
Section 7) caught a real correctness bug in the tiled compute loop:

```cpp
// WRONG — as originally written
sum += a_tile[i][k] * b_tile[k][j];
```

`b_tile` is loaded with the convention `b_tile[j][k]` (first index =
output column / row of `B_T`, second index = the `k` summed over):

```cpp
for (int j = 0; j < TILE; j++)
    for (int k = 0; k < TILE; k++) {
        #pragma HLS PIPELINE II=1
        b_tile[j][k] = B_T[(tj + j) * N + (tk + k)];
    }
```

The compute loop swapped the indices — `b_tile[k][j]` instead of
`b_tile[j][k]` — which silently transposes which dimension is treated
as "row" and which is "sum index" inside the tile. This is wrong on
**75% of outputs** (every index where `j ≠ k`), and diagonal-heavy test
inputs (e.g. identity matrices) can mask it almost entirely, which is
exactly why a patterned, non-symmetric test matrix (`matmul_tb_pattern.cpp`)
was written — a symmetric/identity-only testbench would not reliably
have caught this.

**Fix:**
```cpp
sum += a_tile[i][k] * b_tile[j][k];   // NOT b_tile[k][j]
```

**Verified via C Simulation, both testbenches:**
- `matmul_tb.cpp` — N=16, identity A — PASSED
- `matmul_tb_pattern.cpp` — N=32, patterned/non-symmetric A — PASSED

---

## 9. Hardware Non-Determinism — AXI Write/Read Race on `B_T`

With the `b_tile` bug fixed and C Simulation passing, Member B's
hardware correctness check produced **non-deterministic garbage**:

```
MISMATCH [2]: got 0x7FC00000 (NaN)  expected 0x40400000 (3.0)
MISMATCH [4]: got 0x00000000 (0.0)  expected 0x40A00000 (5.0)
```

Critically, **re-running the identical test produced *different*
garbage each time** — not the same wrong values repeated. That
signature rules out a deterministic logic bug (which the b_tile fix
already addressed and C Simulation already confirmed) and points at a
**race condition**, not a math error.

### Root cause

The kernel's internal `transpose(B, B_T, N)` writes `B_T` to DDR, and
`matmul_tiled(A, B_T, C, N)` immediately reads it back — both through
the same shared `m_axi_ACP` bundle (`A`, `B`, and `B_T` all merged into
one master port per Section 6). `transpose`'s `ap_done` firing only
guarantees its writes were **accepted into the AXI adapter's outstanding
queue** (`num_write_outstanding=16`) — not that they've physically
drained through the AXI4→AXI3 protocol converter and landed in DDR.
`matmul_tiled`'s reads on the same bundle can be issued while writes
from `transpose` are still in flight, with no ordering guarantee
between them. Since real AXI bus arbitration timing varies run to run,
the garbage varies run to run too — consistent with everything observed.

### Two fixes considered

1. **Move the transpose to software** (PS side), with
   `Xil_DCacheFlushRange` as a hard barrier before starting the
   accelerator. Removes the in-kernel write-then-read entirely.
   **Rejected** — kept the transpose in hardware per project scope.
2. **Split `B_T` onto its own `m_axi` bundle with
   `num_write_outstanding=1` / `num_read_outstanding=1`.** Forces the
   AXI adapter to fully drain (BRESP received) before issuing the next
   command on that bundle — making the write-before-read ordering a
   property of the adapter's command queue rather than an assumption
   about `ap_done` timing. **Not yet applied** — see Section 13.

---

## 10. Transpose Burst Optimization (Tile-Buffered Load/Store)

Independent of the race investigation, the original `transpose()` had
a burst-friendliness problem of its own:

```cpp
B_T[j * N + i] = B[i * N + j];   // read bursts (row-sequential),
                                  // write is single-beat (strided by N)
```

The read side (`B[i*N+j]`) is sequential and bursts cleanly; the write
side (`B_T[j*N+i]`) jumps by `N` every element — one AXI beat per
transaction, no burst possible.

**Fix — buffer a 16×16 tile in BRAM, then write it back with the
opposite loop order** so *both* sides become sequential bursts:

```cpp
#define T_TILE 16

void transpose(float *B, float *B_T, int N) {
    #pragma HLS INLINE off
    float tile_buf[T_TILE][T_TILE];

    for (int ti = 0; ti < N; ti += T_TILE) {
        for (int tj = 0; tj < N; tj += T_TILE) {

            // Load — burst read, i outer (matches B's row-major layout)
            for (int i = 0; i < T_TILE; i++)
                for (int j = 0; j < T_TILE; j++) {
                    #pragma HLS PIPELINE II=1
                    tile_buf[i][j] = B[(ti + i) * N + (tj + j)];
                }

            // Store — burst write, j outer (this is the transpose,
            // written out so the DDR write address stays sequential)
            for (int j = 0; j < T_TILE; j++)
                for (int i = 0; i < T_TILE; i++) {
                    #pragma HLS PIPELINE II=1
                    B_T[(tj + j) * N + (ti + i)] = tile_buf[i][j];
                }
        }
    }
}
```

`T_TILE` is independent of `matmul_tiled`'s `TILE` constant — the two
loops interact only through the fully-written `B_T` buffer, so they
can (and do) use different tile sizes with no correctness impact.

**Confirmed at synthesis:** both the load loop (`VITIS_LOOP_33_4`) and
store loop (`VITIS_LOOP_44_6`) now infer 16-beat bursts on bundle
`ACP`, both at II=1. `tile_buf` costs 2 BRAM primitives (~0% of
device). Total design DSP usage barely moved (11 DSPs for transpose,
all address arithmetic) — this is a burst/DDR-traffic fix, not a
compute fix.

**Requires exact tile-size divisibility.** The loop structure assumes
`N % T_TILE == 0`. At small `N` (e.g. `N=4` used in the hardware
correctness check) this means the tile loop still runs a full 16×16
pass over a logically 4×4 region, touching DDR addresses past the
valid matrix — harmless in practice because each buffer's DDR region
is over-provisioned (4MB per matrix), but worth a mental note if
buffer sizing ever changes.

**Known interaction with Section 9:** tiling reduces the *number* of
discrete write transactions to `B_T` by roughly `T_TILE×` (one 16-beat
burst instead of 16 single-beat writes), which likely shrinks the race
window from Section 9 without closing it — see Section 12.

---

## 11. C/RTL Cosimulation — Getting a Real (Not Estimated) Latency Number

Wanted actual cycle-accurate timing for the tiled transpose (HLS's
static Fmax/II figures are pre-place-and-route estimates), which meant
running `cosim_design` for the first time on this kernel. This
surfaced three unrelated bugs before it ran clean — all specific to
how cosim's C-testbench (C-TB) phase works, none of them present in
plain C Simulation.

### Bug 1 — `B_T` array `depth` unspecified on the `m_axi` pragmas

First cosim attempt crashed silently (`SIGSEGV` / Windows
`0xC0000005`, confirmed via `gdb` — see below) inside `transpose`'s
store loop. Root cause: with `N` a runtime argument, HLS cannot
statically know how large the memory behind each `m_axi` pointer needs
to be for the C-TB simulation memory model, and defaults far too
small — cosim's internal scratch buffers for `A`/`B`/`B_T`/`C` came out
only ~128 floats apart (`gdb` showed pointer addresses `0x70fa00`,
`0x70f800`, `0x70f600`, `0x70f400` — 0x200-byte, i.e. 512-byte,
spacing) versus the ~4096 bytes (`N*N` floats) actually needed for
`N=32`. **Purely a simulation-modeling artifact — no effect on real
hardware**, where the accelerator has no array-bounds concept and just
follows AXI addresses.

**Fix — add `depth=MAX_N*MAX_N` to every `m_axi` port:**
```cpp
#pragma HLS INTERFACE m_axi port=A   bundle=ACP offset=slave depth=MAX_N*MAX_N \
    num_read_outstanding=16 max_read_burst_length=16
#pragma HLS INTERFACE m_axi port=B   bundle=ACP offset=slave depth=MAX_N*MAX_N \
    num_read_outstanding=16 max_read_burst_length=16
#pragma HLS INTERFACE m_axi port=B_T bundle=ACP offset=slave depth=MAX_N*MAX_N \
    num_read_outstanding=16 max_read_burst_length=16
#pragma HLS INTERFACE m_axi port=C   bundle=HP0 offset=slave depth=MAX_N*MAX_N \
    num_write_outstanding=16 max_write_burst_length=16
```

### Bug 2 — testbench arrays sized to `N*N`, not `MAX_N*MAX_N`

With `depth` now telling cosim's copy-in step to expect
`MAX_N*MAX_N` (262,144) elements per pointer, it read straight off the
end of the testbench's actual `N*N`-sized (1,024-float) static arrays —
crash moved to `copy_in`/`onebyonecpy_hls.p0a262144f32` (the `262144`
in that symbol name being the giveaway).

**Fix** — size the testbench buffers for the worst case, use only the
first `N*N` elements:
```cpp
#define MAX_N 512   // must match matmul.cpp's MAX_N
static float A[MAX_N*MAX_N], B[MAX_N*MAX_N], B_T[MAX_N*MAX_N],
             C[MAX_N*MAX_N], C_ref[MAX_N*MAX_N];
```

### Diagnostic method — `gdb` on the cosim C-TB executable

Both bugs above produced **no printed output at all** (no `MISMATCH`,
no `TEST PASSED/FAILED`) — a strong signal of a genuine crash rather
than a detected mismatch, since a real mismatch would still reach the
`printf` calls first. Confirmed and located via `gdb` directly on the
generated C-TB executable:
```powershell
cd matmul_hls/solution1/sim/wrapc
& "<vitis-install>/tps/win64/msys64/mingw64/bin/gdb.exe" .\cosim.tv.exe
(gdb) run
(gdb) bt
```
`bt` gave an exact function/line backtrace each time, which is what
made both bugs identifiable in one pass rather than by guessing.

**Environment note:** after killing a crashed `cosim.tv.exe` under
`gdb`, Windows can leave a zombie-looking process holding a file lock
on `sim/wrapc` (`0 handles`, `Access is denied` from a normal
PowerShell). Required an elevated (`Run as Administrator`)
`Stop-Process -Id <pid> -Force` / `taskkill /F /PID <pid>` to actually
clear it before `cosim_design` could regenerate that folder.

---

## 12. Cosimulation Result — PASS, First Real Cycle-Accurate Number

With both bugs above fixed, cosim ran clean end-to-end:

```
C TB testing: TEST PASSED
RTL (Verilog) cosim: Pass
Latency: 69,142 clock cycles (min = avg = max)
```

**What "Pass" confirms:** the actual generated RTL — real AXI burst
transactions, real pipeline fill/drain, real handshaking — produced
bit-identical output to the C model for `N=32`. Stronger evidence than
C Simulation alone, since it exercises real hardware timing behavior,
not just the algorithm.

**Real-time conversion:**
- At the 10ns/100MHz HLS synthesis clock (what cosim ran at):
  `69,142 × 10ns ≈ 691µs`
- At the actual deployed clock (`clk_fpga_0` @ 50MHz / 20ns, per Vivado):
  `69,142 × 20ns ≈ 1.38ms` for `N=32`

**What this does *not* confirm:** cosim uses a behavioral AXI model,
not the real Zynq PS/SCU/protocol-converter path — it cannot reproduce
the Section 9 race. A cosim pass is not evidence the race is fixed;
it's evidence the transpose tiling (Section 10) is functionally and
timing-correct in isolation.

---

## 13. Hardware Re-test with Tiled Transpose — Promising, Not Conclusive

Member B re-ran the hardware correctness check on the newest `.xsa`
(tiled transpose, `TILE=4`, **race fix from Section 9 not yet
applied**):

```
Running matmul correctness check (N=4)...
MATMUL PASS
```

Followed by a full `figures4`-style benchmark sweep (N=32..512, ACP /
HP0 / MATMUL) — all runs at every N passed with no NaN/zero
corruption.

### Why this is good news but not "the race is fixed"

A single clean pass at `N=4` is weak evidence for a probabilistic race.
Per Section 10, tiling cut the number of discrete `B_T` write
transactions by roughly `T_TILE×` — at `N=4` this shrinks an already
tiny transaction count to almost nothing, which could produce a clean
pass simply because the race window got very small, **without the
underlying ordering guarantee actually existing**. The repeated-run,
larger-N sweep is stronger evidence than the N=4 check alone, but the
bundle-split fix (Section 9, option 2) still hasn't been applied — this
should be treated as "probably improved, not architecturally resolved"
until that fix is in and re-tested, or until many repeated runs at
larger N (64, 128, 256) show zero flakiness.

### Performance finding from the same sweep — `TILE=4` reuse bottleneck

```
MATMUL,512,7113172,...   (~7.11 seconds at N=512)
```

Worked backward from per-loop synthesis latencies:
- Transpose (T_TILE=16): `(512/16)² × 538 cycles ≈ 551K cycles ≈ 11ms`
  — negligible, confirms Section 10's fix isn't the bottleneck.
- `matmul_tiled` (TILE=4): per `tk` step, A-tile load (~29 cyc) +
  B_T-tile load (~29 cyc) + compute (~35 cyc) ≈ 93 cycles, run
  sequentially (no load/compute overlap). For N=512: `128 tk-steps ×
  16,384 output tiles × ~93 cycles ≈ 196M cycles ≈ 3.9s` at the ideal
  HLS latency estimate — same order of magnitude as the measured 7.11s,
  the gap attributable to real AXI/SCU round-trip overhead the static
  per-loop II estimate doesn't model.

**Root cause: `TILE=4` gives poor DDR-transaction reuse** — every `tk`
step fetches a 4×4 tile (16 elements) of `A` and `B_T` to do 16
multiply-accumulates: one DDR round trip per useful MAC, with nothing
to hide the round-trip latency behind (load and compute run strictly
sequentially, no double-buffering).

**Next optimization identified, not yet applied:** increase `TILE`
(current DSP usage is 108/220, plenty of headroom for `TILE=8` →
~64 compute DSPs, 4× better reuse-per-transaction) and/or add
ping-pong double-buffering between the load and compute stages so the
next tile's load overlaps the current tile's compute instead of
sitting in series on the critical path.

### Updated status checklist (supersedes Section 7)

```
✓ b_tile indexing bug found (C Sim) and fixed
✓ AXI write/read race on B_T diagnosed (root cause understood,
  fix designed, not yet applied — Section 9)
✓ Transpose re-optimized for burst read+write via tile buffering
  (Section 10)
✓ C/RTL cosimulation run successfully; transpose timing validated
  at cycle-accurate RTL level (Section 11–12)
✓ Hardware N=4 correctness check passing on latest .xsa (promising,
  not conclusive re: the race — Section 13)
✓ figures4-style sweep (N=32..512) run, no corruption observed at
  any N tested
□ Pending: apply Section 9's bundle-split fix (num_write_outstanding=1
  on a dedicated B_T bundle) and re-test at larger N with repeated runs
  before treating the race as resolved
□ Pending: TILE=4 → TILE=8 (or double-buffering) to address the
  ~7.1s @ N=512 performance bottleneck identified above
□ Pending: final report incorporating all four benchmark rounds
```

---

## 14. `TILE=8` Attempt — DSP Overflow, and Why the Estimate Was Wrong

With the `FETCH_TILE=8`/`TILE=4` bottleneck from Section 13 identified,
the first thing tried was simply increasing `TILE` (the fully-unrolled
compute engine) from 4 to 8, on the assumption that DSP cost scales
roughly linearly with `TILE` (the ~64-DSP estimate cited at the end of
Section 13).

**That assumption was wrong.** `place_design` failed immediately:
```
DRC UTLZ-1: This design requires 336 of such cell types but only 220
compatible sites are available in the target device.
```

Checking actual synthesis numbers against `TILE` size:

| TILE | Unrolled MACs (TILE²) | Actual DSPs | Ratio |
|---|---|---|---|
| 4 | 16 | 97 | ≈5.5× |
| 8 | 64 | 336 | ≈5.25× |
| 16 | 256 | 1312 (Section 5) | ≈5.1× |

**DSP cost scales as `TILE²`, not `TILE`** — both `j` and `k` are fully
unrolled in the compute loop, so the number of parallel `fmul`/`fadd`
units is `TILE (j) × TILE (k)`. The ≈5× DSPs-per-MAC multiplier is
consistent across all three data points (the `fmul` core costs ~3
DSPs, the `fadd` reduction tree ~2, per unrolled element). There is no
power-of-2 `TILE` between 4 and 8 to fall back to — `TILE=8` is simply
~3.4× over budget, not a small overshoot.

Two options considered: (A) switch to a leaner DSP-per-MAC
implementation via `#pragma HLS BIND_OP ... impl=meddsp/fabric`
(trades LUTs, which are only 41% used, for fewer DSPs, but doesn't fix
the actual DDR-round-trip problem), or (B) decouple "how much data is
fetched per DDR trip" from "how many DSPs are used," structurally.
**Option B chosen** — see Section 15.

---

## 15. `FETCH_TILE=8` — Decoupling DDR Fetch Size from Compute Engine Size

The insight: keep the existing `TILE=4` compute engine exactly as-is
(same DSP budget, same unrolled `j`/`k` structure that gave II=1 back
in Section 4), but fetch a **bigger** block per DDR round-trip
(`FETCH_TILE=8`, i.e. 4× the data), and loop over four `TILE=4`
sub-blocks of that bigger fetched tile to consume it:

```cpp
#define TILE        4    // compute sub-tile — DSP budget, unchanged
#define FETCH_TILE  8    // DDR fetch tile — data per round-trip
                          // FETCH_TILE must be a multiple of TILE
                          // N must be a multiple of FETCH_TILE

// per tk step: fetch FETCH_TILE x FETCH_TILE of A and B_T (one
// larger burst instead of TILE=4's smaller one), then consume it
// via nested si/sj/sk loops over TILE-sized sub-blocks:
for (int si = 0; si < FETCH_TILE; si += TILE) {
    for (int sj = 0; sj < FETCH_TILE; sj += TILE) {
        for (int sk = 0; sk < FETCH_TILE; sk += TILE) {
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

This cuts `tk`-iterations (the thing that costs a DDR round-trip each
time) from `N/TILE` to `N/FETCH_TILE` — at N=512, 128 → 64 per output
tile, ~8× fewer total round-trips across the whole matmul
(`(FETCH_TILE/TILE)³`), while the actual unrolled MAC engine — and
therefore DSP count — stays unchanged.

---

## 16. II=2 Violation from Automatic Loop Flattening — Root Cause and Fix

First synthesis attempt at the Section 15 structure passed C
Simulation but **failed the loop constraint check**:
```
WARNING: [HLS 200-880] The II Violation ...: Unable to enforce a
carried dependence constraint (II = 1, distance = 4, offset = 1)
between 'store' ... and 'load' ... on array 'c_tile'.
Pipelining result : Target II = 1, Final II = 2, Depth = 33
INFO: [HLS 200-790] **** Loop Constraint Status: All loop constraints
were NOT satisfied.
```

**Root cause:** `sk`'s loop body contains exactly one child loop (the
`i` loop) — a perfect nest, which is precisely the shape HLS's
automatic loop-flattening looks for. It silently merged `sk` straight
into the `i`/`j`/`k` pipeline, with no warning, into one continuous
II=1 iteration stream. That meant consecutive `sk` values were treated
as just more iterations of the same pipeline with no boundary between
them — so `sk=0`'s write to `c_tile[si+i][sj+j]` could still be in
flight when `sk=4`'s read of that *same* address tried to start
(distance=4, matching `FETCH_TILE/TILE`), since the read-modify-write
accumulation `c_tile[si+i][sj+j] += sum` revisits the same cell once
per `sk` sub-block by design.

**Why the original (non-sub-tiled) kernel never hit this:** its `tk`
loop wraps *three* sibling sub-loops (load A, load B_T, compute) —
`"Cannot flatten loop ... more than one sub loop"` — so HLS refuses to
auto-flatten it with anything beneath it, for free, purely as a side
effect of that loop body's shape. `sk` has no such structural
protection, since its body is a single clean child loop.

**Fix — force the boundary explicitly, since it isn't free here:**
```cpp
for (int sk = 0; sk < FETCH_TILE; sk += TILE) {
    #pragma HLS LOOP_FLATTEN off
    for (int i = 0; i < TILE; i++) {
        #pragma HLS PIPELINE II=1
        ...
```
`LOOP_FLATTEN off` makes each `(si,sj,sk)` combination run its
pipeline to completion — including draining the `c_tile` write —
before the next `sk` starts, at the cost of a drain/refill penalty at
each `sk` boundary (small and bounded, since `sk` only has
`FETCH_TILE/TILE = 2` iterations).

---

## 17. Synthesis Confirmation — Fix Verified

Re-running C Synthesis after Section 16's fix:
```
INFO: [HLS 200-790] **** Loop Constraint Status: All loop constraints
were satisfied.
```
- Compute pipeline (`VITIS_LOOP_120_13`, the `i` loop only — `si`/
  `sj`/`sk` no longer merged in): `Target II = 1, Final II = 1, Depth
  = 32` — matching the original `TILE=4` engine's depth almost exactly.
- DSPs: `16 fmul` + `20 fadd` instances — `16 = TILE² = 4²`, confirming
  DSP count returned to the clean `TILE=4` baseline (~97 total,
  comfortably under 220), not the artificially-halved count the
  broken II=2 schedule had shown.
- Burst lengths unaffected: A/B_T loads still inferring length-8
  bursts (`FETCH_TILE=8`) — the actual performance goal survived the
  correction intact.
- Fmax unchanged at 136.99 MHz — no timing regression.

---

## 18. Cosimulation — Both Testbenches Validated (With Two Detours)

**`matmul_tb.cpp` (identity, N=16):** first cosim attempt after this
change hit a plain compile error —
```
error: 'C_ref' was not declared in this scope
```
— an editing slip from the earlier `MAX_N*MAX_N` resizing pass
(Section 11); the array declaration line had dropped `C_ref` for this
file specifically. Restored, re-ran: **cosim PASS.**

**`matmul_tb_pattern.cpp` (N=32, patterned):** cosim console showed
`TEST PASSED` / `*** C/RTL co-simulation finished: PASS ***`, but the
Vitis HLS GUI's Cosim Report panel displayed `Fail` immediately after
— traced to a **stale cached view** in the GUI (it had been showing a
result from an earlier, genuinely-failed run of the same testbench and
didn't auto-refresh). Confirmed via direct file read + timestamp check
that the on-disk report matched the passing console run:
```
Verilog | Pass | Latency: 42,830 cycles (min=avg=max)
```
— down from the pre-`FETCH_TILE=8` baseline of 69,142 cycles at the
same N=32, a real ~38% cycle-count reduction confirmed at the
cycle-accurate RTL level, consistent with (though not directly
proportional to, since fixed overhead is a larger fraction of the
total at small N) the ~8× round-trip reduction from Section 15.

**Lesson:** when the GUI and a freshly-generated report file disagree,
trust the file (`solution1/sim/report/matmul_cosim.rpt`) and its
`LastWriteTime`, not the GUI panel, which can hold a cached view from
a prior run.

---

## 19. Hardware Validation — N=4 Boundary Bug, N≥32 Clean, Real Speedup Confirmed

Member B's first hardware re-test (still using the old N=4
correctness-check convention) failed:
```
MISMATCH [0]: got 0x40E00000 expected 0x3F800000  (7.0 vs 1.0)
MISMATCH [1]: got 0x41100000 expected 0x40000000  (9.0 vs 2.0)
...
MATMUL FAIL (16 errors)
```

**This was correctly diagnosed as a new, deterministic bug — not the
Section 9 race.** Decoding the mismatches: `got = 2×expected + 5`
exactly, across the first 11 indices — a clean affine relationship, the
opposite signature of the AXI race (which produces NaN/zero, different
garbage on every re-run, no algebraic structure). Indices 11–15 showed
values consistent with reading uninitialized DDR memory entirely.

**Root cause:** `N=4 < FETCH_TILE=8`. The `ti`/`tj` outer loop still
executes once, but the inner load/compute loops always run their full
`FETCH_TILE=8` extent regardless of `N` — so the kernel fetched an 8×8
block and computed an 8×8 output tile against a logically 4×4 matrix,
reading past the valid region into adjacent/uninitialized memory. Same
category of bug as the `T_TILE=16`-at-`N=4` case noted back in
Section 10, except this time the over-fetch was large enough relative
to the matrix to produce visible corruption rather than landing
harmlessly in over-provisioned buffer padding.

**Checked against the actual benchmark sweep sizes** — N=32, 64, 128,
256, 512 are all multiples of 16 (the largest of `FETCH_TILE=8`,
`TILE=4`, `T_TILE=16`), so all divide cleanly; N=4 was the only tested
size smaller than `FETCH_TILE` itself. **Fix applied: correctness
check updated from N=4 to N=32** (documented in
`figures5-hwAccelerator3-tiling-v2/README`), rather than special-casing
small-N in the kernel, since N=4 was only ever a smoke test and the
real sweep never uses it.

**Re-test at N=32 — clean:**
```
Running matmul correctness check (N=32)...
MATMUL PASS
```
No corruption at any size in the subsequent full sweep (32–512).

**Real performance result — corrected numbers** (an earlier draft of
the `figures5` README was generated from a mismatched/stale log and
overstated HW v3 latency by 33–40% at every N; recalculated directly
from the UART sweep log's `MATMUL,N,elapsed_us` rows):

```
  N     HW v2 (us)   HW v3 (us)   Speedup (v2/v3)
  ---   ----------   ----------   ---------------
   32        1,985        1,194           1.66x
   64       14,776        8,545           1.73x
  128      114,070       64,588           1.77x
  256      896,958      502,071           1.79x
  512    7,113,180    3,962,935           1.80x
```

At N=512: **~7.11s → ~3.96s**, a genuine ~44% reduction, close to the
~3.9s figure derived analytically from per-loop synthesis latencies
back in Section 13 — real hardware landing near the ideal-latency
estimate is a good sign the `FETCH_TILE=8` mental model (fewer, bigger
DDR round-trips) is actually correct, not just plausible-sounding.
HW v3 vs SW matmul speedup ranges 2.0–2.5× across N=32–512 (see
`figures5-hwAccelerator3-tiling-v2/README` for full SW comparison and
cache-behavior figures).

**Still outstanding — not touched by any of this session's work:**
the Section 9 AXI write/read race on `B_T` (shared-bundle,
`num_write_outstanding=16`, no drain guarantee between `transpose`'s
writes and `matmul_tiled`'s reads). All testing in Sections 17–19
validates correctness and performance of `FETCH_TILE=8` specifically;
none of it constitutes evidence for or against the race, since a small
number of passing runs at moderate N was already established (Section
13) as weak evidence either way for a probabilistic race. The
bundle-split fix (`num_write_outstanding=1` on a dedicated `B_T`
bundle) remains designed but unapplied.

### Updated status checklist (supersedes Section 13)

```
✓ b_tile indexing bug found (C Sim) and fixed
✓ AXI write/read race on B_T diagnosed (root cause understood, fix
  designed, STILL NOT APPLIED — Section 9)
✓ Transpose re-optimized for burst read+write via tile buffering
  (Section 10)
✓ TILE=8 attempted, correctly diagnosed as DSP-infeasible (Section 14)
✓ FETCH_TILE=8 sub-tiling implemented, decoupling DDR fetch size from
  compute DSP budget (Section 15)
✓ II=2 violation from automatic loop flattening found and fixed via
  LOOP_FLATTEN off (Section 16), reconfirmed at II=1 (Section 17)
✓ Both testbenches (identity N=16, patterned N=32) passing cosim at
  the cycle-accurate RTL level (Section 18)
✓ N=4 boundary bug found and correctly root-caused (N < FETCH_TILE);
  correctness check moved to N=32 (Section 19)
✓ Hardware validated clean at N=32..512, no corruption at any tested
  size
✓ Real ~44% latency reduction at N=512 confirmed on hardware
  (7.11s → 3.96s), matching analytical prediction
✓ figures5 benchmark README corrected after a stale-log discrepancy
  was caught (33-40% overstatement at every N in the first draft)
□ Pending: apply Section 9's bundle-split fix and re-test at larger N
  with many repeated runs before treating the race as resolved —
  still the single largest open correctness risk in the project
□ Pending: final report incorporating all five benchmark rounds
```

---

## 20. File Locations

```
matmul_hls/matmul.cpp     HLS source (transpose + tiled matmul kernel)
matmul_hls/export.zip     Exported Vivado IP (regenerate after each
                           HLS change — re-export, don't reuse a stale zip)
project_2/                Vivado project (block design: system.bd)
```