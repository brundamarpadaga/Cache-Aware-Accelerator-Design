#!/usr/bin/env python3
"""
plot_results.py — ACP vs HP0 vs SW-Matmul vs HW-Matmul Benchmark Result Plotter
================================================================================
Board:   Digilent Zybo Z7-20 (XC7Z020-1CLG400C)
Project: ACP / HP0 / SW matmul / HW matmul (Vitis HLS accelerator)

Usage
-----
  # Single UART log file:
  python3 plot_results.py results.log --out figures/

  # Multiple log files (merged onto the same plots):
  python3 plot_results.py dma_baseline.log results_with_sw.log --out figures/

  # Pipe directly from a serial terminal:
  python3 plot_results.py -          # reads stdin

Expected CSV input format (lines starting with '#' are ignored as comments):
  mode,N,elapsed_us,l1d_miss,l1d_access,l2d_miss,l2d_access,l1_hit_pct,l2_hit_pct

  mode        : "ACP", "HP", "SW", or "MATMUL"
  N           : matrix side length (32, 64, 128, 256, 512)
  elapsed_us  : wall-clock time in microseconds
                  ACP    — DMA loopback via ACP, no cache management overhead
                  HP     — DMA loopback via HP0, includes SRC flush + DST invalidate
                  SW     — full software N×N matmul on ARM CPU (A×B→C, cold-cache)
                  MATMUL — Vitis HLS accelerator matmul_0; times ap_start → ap_done
                           including DST cache invalidate; A+B flushed before start
  l1d_miss    : L1 D-cache miss count (ARM PMU CTR0, event 0x03)
  l1d_access  : L1 D-cache access count (ARM PMU CTR1, event 0x04)
  l2d_miss    : L2 miss count derived from PL310 (drreq - drhit)
  l2d_access  : L2 access count from PL310 DRREQ counter
  l1_hit_pct  : L1 hit rate as float 0–100 (computed on PS, printed xx.yy)
  l2_hit_pct  : L2 hit rate as float 0–100 (computed on PS, printed xx.yy)

Multiple rows per (mode, N) are produced by REPEATS=5 runs; this script
plots the median value for each (mode, N) pair with min/max error bars.
"""

import sys
import argparse
import csv
import io
import os
from collections import defaultdict
from statistics import median

# ---------------------------------------------------------------------------
# Optional dependency check — give a clear message if matplotlib is missing
# ---------------------------------------------------------------------------
try:
    import matplotlib
    matplotlib.use("Agg" if "DISPLAY" not in os.environ else "TkAgg")
    import matplotlib.pyplot as plt
    import matplotlib.ticker as ticker
    HAS_MPL = True
except ImportError:
    HAS_MPL = False
    print("WARNING: matplotlib not found — install with:  pip install matplotlib")
    print("         Falling back to text-only summary output.\n")

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
EXPECTED_SIZES  = [32, 64, 128, 256, 512]
MODES           = ["ACP", "HP", "SW", "MATMUL"]
COLORS          = {"ACP": "#1f77b4", "HP": "#d62728", "SW": "#2ca02c", "MATMUL": "#9467bd"}
MARKERS         = {"ACP": "o",       "HP": "s",       "SW": "^",       "MATMUL": "D"}

FIELDNAMES = [
    "mode", "N", "elapsed_us",
    "l1d_miss", "l1d_access",
    "l2d_miss", "l2d_access",
    "l1_hit_pct", "l2_hit_pct",
]

# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------

def parse_log(source) -> list[dict]:
    """
    Extract CSV rows from a UART log.

    Accepts mixed output — log lines, xil_printf debug prints, blank lines,
    and comment lines starting with '#' are all silently skipped.
    Only lines whose first field is 'ACP', 'HP', or 'SW' are kept.
    """
    rows = []

    if source == "-":
        raw = sys.stdin.read()
    else:
        with open(source, "r", errors="replace") as f:
            raw = f.read()

    # Strip Windows CR and feed through csv reader
    reader = csv.DictReader(
        io.StringIO(raw.replace("\r\n", "\n").replace("\r", "\n")),
        fieldnames=FIELDNAMES,
    )

    lineno = 0
    skipped = 0
    for row in reader:
        lineno += 1
        mode = row.get("mode", "").strip().upper()
        if mode not in ("ACP", "HP", "SW", "MATMUL"):
            skipped += 1
            continue
        try:
            parsed = {
                "mode":        mode,
                "N":           int(row["N"].strip()),
                "elapsed_us":  float(row["elapsed_us"].strip()),
                "l1d_miss":    int(row["l1d_miss"].strip()),
                "l1d_access":  int(row["l1d_access"].strip()),
                "l2d_miss":    int(row["l2d_miss"].strip()),
                "l2d_access":  int(row["l2d_access"].strip()),
                "l1_hit_pct":  float(row["l1_hit_pct"].strip()),
                "l2_hit_pct":  float(row["l2_hit_pct"].strip()),
            }
            rows.append(parsed)
        except (ValueError, KeyError) as exc:
            print(f"  [parse] line {lineno}: skipping malformed row — {exc}")
            skipped += 1
            continue

    print(f"Parsed {len(rows)} data rows ({skipped} non-data lines skipped).")
    return rows


# ---------------------------------------------------------------------------
# Aggregation  (median + min/max per mode × N)
# ---------------------------------------------------------------------------

def aggregate(rows: list[dict]) -> dict:
    """
    Returns a nested dict:
      agg[mode][field] = list of (N, median_val, min_val, max_val)
    sorted by N.
    """
    # Bucket raw values
    buckets: dict[str, dict[int, dict[str, list]]] = defaultdict(
        lambda: defaultdict(lambda: defaultdict(list))
    )
    for r in rows:
        for field in FIELDNAMES[2:]:   # skip mode, N
            buckets[r["mode"]][r["N"]][field].append(r[field])

    agg: dict[str, dict[str, list]] = {}
    for mode in MODES:
        if mode not in buckets:
            continue
        agg[mode] = defaultdict(list)
        for N in sorted(buckets[mode].keys()):
            for field in FIELDNAMES[2:]:
                vals = buckets[mode][N][field]
                agg[mode][field].append((
                    N,
                    median(vals),
                    min(vals),
                    max(vals),
                ))

    return agg


def unzip_series(series: list[tuple]) -> tuple[list, list, list, list]:
    """Split [(N, med, lo, hi), ...] → (Ns, meds, los, his)."""
    Ns   = [t[0] for t in series]
    meds = [t[1] for t in series]
    los  = [t[2] for t in series]
    his  = [t[3] for t in series]
    return Ns, meds, los, his


# ---------------------------------------------------------------------------
# Text summary (always printed, even without matplotlib)
# ---------------------------------------------------------------------------

def print_summary(agg: dict) -> None:
    print()
    print("=" * 70)
    print("  BENCHMARK SUMMARY  (median of repeated runs)")
    print("=" * 70)

    header = f"{'Mode':<6} {'N':>5} {'elapsed_us':>12} {'L1 hit%':>9} {'L2 hit%':>9}"
    print(header)
    print("-" * len(header))

    for mode in MODES:
        if mode not in agg:
            print(f"  {mode}: no data")
            continue
        for entry in agg[mode].get("elapsed_us", []):
            N, med_us, _, _ = entry
            l1_series = agg[mode].get("l1_hit_pct", [])
            l2_series = agg[mode].get("l2_hit_pct", [])
            # find matching N
            l1_hit = next((t[1] for t in l1_series if t[0] == N), float("nan"))
            l2_hit = next((t[1] for t in l2_series if t[0] == N), float("nan"))
            print(f"{mode:<6} {N:>5} {med_us:>12.1f} {l1_hit:>9.2f} {l2_hit:>9.2f}")
        print()

    # Speedup column
    if "ACP" in agg and "HP" in agg:
        print("-" * len(header))
        print(f"{'':6} {'N':>5} {'ACP/HP ratio':>12}   (lower = ACP faster)")
        print("-" * len(header))
        acp_us = {t[0]: t[1] for t in agg["ACP"].get("elapsed_us", [])}
        hp_us  = {t[0]: t[1] for t in agg["HP"].get("elapsed_us", [])}
        for N in sorted(set(acp_us) & set(hp_us)):
            ratio = acp_us[N] / hp_us[N] if hp_us[N] else float("nan")
            faster = "ACP faster" if ratio < 1.0 else "HP faster"
            print(f"{'':6} {N:>5} {ratio:>12.3f}   ({faster})")
        print()

    print("=" * 70)
    print()


# ---------------------------------------------------------------------------
# Plotting helpers
# ---------------------------------------------------------------------------

def _errorbars(ax, series, mode, **kwargs):
    """Plot median line + min/max shading for one mode."""
    Ns, meds, los, his = unzip_series(series)
    yerr_lo = [m - lo for m, lo in zip(meds, los)]
    yerr_hi = [hi - m for m, hi in zip(meds, his)]
    color  = COLORS[mode]
    marker = MARKERS[mode]
    line, caps, bars = ax.errorbar(
        Ns, meds,
        yerr=[yerr_lo, yerr_hi],
        label=mode,
        color=color,
        marker=marker,
        markersize=6,
        linewidth=1.8,
        capsize=4,
        elinewidth=1.2,
        **kwargs,
    )
    ax.fill_between(Ns, los, his, color=color, alpha=0.12)
    return line


def _style_xaxis(ax):
    ax.set_xscale("log", base=2)
    ax.set_xticks(EXPECTED_SIZES)
    ax.xaxis.set_major_formatter(ticker.ScalarFormatter())
    ax.set_xlabel("Matrix side N")
    ax.grid(True, which="both", linestyle="--", alpha=0.4)
    ax.legend(framealpha=0.9)


# ---------------------------------------------------------------------------
# Figure 1 — Transfer Latency
# ---------------------------------------------------------------------------

def plot_latency(agg: dict, out_path: str | None) -> None:
    fig, ax = plt.subplots(figsize=(7, 4.5))

    for mode in MODES:
        if mode not in agg or not agg[mode].get("elapsed_us"):
            continue
        _errorbars(ax, agg[mode]["elapsed_us"], mode)

    ax.set_ylabel("Elapsed time (µs)")
    ax.set_title("ACP vs HP0 vs SW-Matmul vs HW-Matmul — Latency\n(median ± min/max over 5 repeats)")
    _style_xaxis(ax)

    # Annotate which cost is included per mode
    ax.annotate(
        "HP: DMA loopback — includes SRC flush + DST invalidate",
        xy=(0.02, 0.97), xycoords="axes fraction",
        va="top", fontsize=7.5, color=COLORS["HP"],
    )
    ax.annotate(
        "ACP: DMA loopback — no flush/invalidate, SCU handles coherency",
        xy=(0.02, 0.91), xycoords="axes fraction",
        va="top", fontsize=7.5, color=COLORS["ACP"],
    )
    ax.annotate(
        "SW: software N×N matmul on ARM CPU (cold-cache)",
        xy=(0.02, 0.85), xycoords="axes fraction",
        va="top", fontsize=7.5, color=COLORS["SW"],
    )
    ax.annotate(
        "MATMUL: Vitis HLS accelerator — ap_start to ap_done + DST invalidate",
        xy=(0.02, 0.79), xycoords="axes fraction",
        va="top", fontsize=7.5, color=COLORS["MATMUL"],
    )

    fig.tight_layout()
    _save_or_show(fig, out_path, "latency.png")


# ---------------------------------------------------------------------------
# Figure 2 — L1 D-cache Hit Rate
# ---------------------------------------------------------------------------

def plot_l1_hit(agg: dict, out_path: str | None) -> None:
    fig, ax = plt.subplots(figsize=(7, 4.5))

    for mode in MODES:
        if mode not in agg or not agg[mode].get("l1_hit_pct"):
            continue
        _errorbars(ax, agg[mode]["l1_hit_pct"], mode)

    ax.set_ylabel("L1 D-cache hit rate (%)")
    ax.set_ylim(0, 105)
    ax.set_title("ACP vs HP0 vs SW vs MATMUL — L1 D-Cache Hit Rate\n(ARM PMU events 0x03 / 0x04)")
    _style_xaxis(ax)

    fig.tight_layout()
    _save_or_show(fig, out_path, "l1_hit_rate.png")


# ---------------------------------------------------------------------------
# Figure 3 — L2 Hit Rate  (PL310 DRREQ / DRHIT)
# ---------------------------------------------------------------------------

def plot_l2_hit(agg: dict, out_path: str | None) -> None:
    fig, ax = plt.subplots(figsize=(7, 4.5))

    for mode in MODES:
        if mode not in agg or not agg[mode].get("l2_hit_pct"):
            continue
        _errorbars(ax, agg[mode]["l2_hit_pct"], mode)

    ax.set_ylabel("L2 hit rate (%)  [PL310 DRHIT / DRREQ]")
    ax.set_ylim(0, 105)
    ax.set_title("ACP vs HP0 vs SW vs MATMUL — PL310 L2 Cache Hit Rate\n(MMIO counters at 0xF8F02000)")
    _style_xaxis(ax)

    fig.tight_layout()
    _save_or_show(fig, out_path, "l2_hit_rate.png")


# ---------------------------------------------------------------------------
# Figure 4 — ACP vs HP speedup ratio
# ---------------------------------------------------------------------------

def plot_speedup(agg: dict, out_path: str | None) -> None:
    """Plot HP/ACP ratio and SW/ACP ratio so the cost of software compute is visible."""
    if "ACP" not in agg:
        return
    acp_us = {t[0]: t[1] for t in agg["ACP"].get("elapsed_us", [])}
    if not acp_us:
        return

    fig, ax = plt.subplots(figsize=(7, 4.5))
    ax.axhline(1.0, color="gray", linestyle="--", linewidth=1, label="break-even (= ACP)")

    if "HP" in agg:
        hp_us = {t[0]: t[1] for t in agg["HP"].get("elapsed_us", [])}
        common = sorted(set(acp_us) & set(hp_us))
        if common:
            ratios = [hp_us[N] / acp_us[N] for N in common]
            ax.plot(common, ratios, marker="s", color=COLORS["HP"],
                    linewidth=2, markersize=7, label="HP / ACP")
            ax.fill_between(common, 1.0, ratios,
                            where=[r > 1 for r in ratios],
                            alpha=0.12, color=COLORS["HP"])

    if "SW" in agg:
        sw_us = {t[0]: t[1] for t in agg["SW"].get("elapsed_us", [])}
        common = sorted(set(acp_us) & set(sw_us))
        if common:
            ratios = [sw_us[N] / acp_us[N] for N in common]
            ax.plot(common, ratios, marker="^", color=COLORS["SW"],
                    linewidth=2, markersize=7, label="SW matmul / ACP")
            ax.fill_between(common, 1.0, ratios,
                            where=[r > 1 for r in ratios],
                            alpha=0.12, color=COLORS["SW"])

    if "MATMUL" in agg:
        matmul_us = {t[0]: t[1] for t in agg["MATMUL"].get("elapsed_us", [])}
        common = sorted(set(acp_us) & set(matmul_us))
        if common:
            ratios = [matmul_us[N] / acp_us[N] for N in common]
            ax.plot(common, ratios, marker="D", color=COLORS["MATMUL"],
                    linewidth=2, markersize=7, label="HW matmul / ACP")
            ax.fill_between(common, 1.0, ratios,
                            where=[r > 1 for r in ratios],
                            alpha=0.12, color=COLORS["MATMUL"])

    ax.set_ylabel("Mode elapsed / ACP elapsed  (> 1 = ACP faster)")
    ax.set_title("Relative Cost vs ACP DMA Baseline\n(HP, SW, MATMUL normalised to ACP time)")
    _style_xaxis(ax)
    ax.legend(framealpha=0.9)

    fig.tight_layout()
    _save_or_show(fig, out_path, "speedup.png")


# ---------------------------------------------------------------------------
# Figure 5 — L1 miss counts (raw)
# ---------------------------------------------------------------------------

def plot_l1_misses(agg: dict, out_path: str | None) -> None:
    fig, ax = plt.subplots(figsize=(7, 4.5))

    for mode in MODES:
        if mode not in agg or not agg[mode].get("l1d_miss"):
            continue
        _errorbars(ax, agg[mode]["l1d_miss"], mode)

    ax.set_ylabel("L1 D-cache misses (count)")
    ax.set_title("ACP vs HP0 vs SW vs MATMUL — L1 D-Cache Miss Count\n(ARM PMU event 0x03)")
    _style_xaxis(ax)

    fig.tight_layout()
    _save_or_show(fig, out_path, "l1_misses.png")


# ---------------------------------------------------------------------------
# Save / show helper
# ---------------------------------------------------------------------------

def _save_or_show(fig, out_dir: str | None, filename: str) -> None:
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
        path = os.path.join(out_dir, filename)
        fig.savefig(path, dpi=150)
        print(f"  Saved: {path}")
        plt.close(fig)
    else:
        plt.show()
        plt.close(fig)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Plot ACP vs HP0 vs SW-Matmul benchmark results from Zynq UART log(s)."
    )
    parser.add_argument(
        "sources",
        nargs="*",
        default=["-"],
        help="Path(s) to UART log file(s), or '-' for stdin (default: stdin). "
             "Multiple files are merged — useful for comparing a DMA-only run "
             "with a later SW-matmul run.",
    )
    parser.add_argument(
        "--out", "-o",
        metavar="DIR",
        default=None,
        help="Save PNG figures to DIR instead of showing interactively",
    )
    args = parser.parse_args()

    rows = []
    for src in args.sources:
        rows.extend(parse_log(src))

    if not rows:
        print("ERROR: No valid data rows found.")
        print("  Make sure the UART log contains lines like:")
        print("    ACP,64,1234,5678,90123,456,789,94.50,82.30")
        print("    SW,64,98765,12345,67890,234,567,81.20,54.30")
        print("    MATMUL,64,456,789,12345,67,890,95.10,88.40")
        sys.exit(1)

    agg = aggregate(rows)
    print_summary(agg)

    if not HAS_MPL:
        print("Install matplotlib to generate charts:  pip install matplotlib")
        return

    print("Generating plots...")
    plot_latency  (agg, args.out)
    plot_l1_hit   (agg, args.out)
    plot_l2_hit   (agg, args.out)
    plot_speedup  (agg, args.out)
    plot_l1_misses(agg, args.out)
    print("Done.")


if __name__ == "__main__":
    main()