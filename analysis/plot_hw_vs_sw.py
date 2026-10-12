#!/usr/bin/env python3
"""
plot_hw_vs_sw.py  —  HW accelerator vs SW matmul comparison
============================================================
Pulls MATMUL rows from results_with_freq100Hz.txt and SW rows from
results_with_sw2.txt, then generates a latency and speedup chart.

Usage:
  python analysis/plot_hw_vs_sw.py
"""

import os
import sys
from collections import defaultdict
from statistics import median

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import matplotlib.ticker as ticker
except ImportError:
    print("ERROR: pip install matplotlib")
    sys.exit(1)

SIZES = [32, 64, 128, 256, 512]
HERE  = os.path.dirname(os.path.abspath(__file__))

SOURCES = {
    "MATMUL": os.path.join(HERE, "results_with_freq100Hz.txt"),
    "SW":     os.path.join(HERE, "results_with_sw2.txt"),
}

COLORS  = {"MATMUL": "#9467bd", "SW": "#2ca02c"}
MARKERS = {"MATMUL": "D",       "SW": "^"}
LABELS  = {"MATMUL": "HW  (matmul_0 HLS, 100 MHz PL)",
           "SW":     "SW  (ARM i-k-j loop, 650 MHz)"}


def parse(path, mode):
    us_by_N = defaultdict(list)
    with open(path, errors='replace') as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split(',')
            if len(parts) < 3 or parts[0] != mode:
                continue
            try:
                N  = int(parts[1])
                us = int(parts[2])
            except ValueError:
                continue
            if N in SIZES and us > 0:
                us_by_N[N].append(us)
    return {N: (median(v), min(v), max(v)) for N, v in us_by_N.items()}


def _errorbars(ax, data, mode):
    Ns   = sorted(data)
    meds = [data[N][0] for N in Ns]
    los  = [data[N][0] - data[N][1] for N in Ns]
    his  = [data[N][2] - data[N][0] for N in Ns]
    ax.errorbar(Ns, meds, yerr=[los, his],
                label=LABELS[mode], color=COLORS[mode], marker=MARKERS[mode],
                markersize=6, linewidth=1.8, capsize=4, elinewidth=1.2)
    ax.fill_between(Ns,
                    [data[N][1] for N in Ns],
                    [data[N][2] for N in Ns],
                    color=COLORS[mode], alpha=0.12)


def _xaxis(ax):
    ax.set_xscale('log', base=2)
    ax.set_xticks(SIZES)
    ax.xaxis.set_major_formatter(ticker.ScalarFormatter())
    ax.set_xlabel('Matrix side  N')
    ax.grid(True, which='both', linestyle='--', alpha=0.4)
    ax.legend(framealpha=0.9)


def main():
    hw = parse(SOURCES["MATMUL"], "MATMUL")
    sw = parse(SOURCES["SW"],     "SW")

    if not hw:
        print(f"ERROR: no MATMUL rows found in {SOURCES['MATMUL']}")
        sys.exit(1)
    if not sw:
        print(f"ERROR: no SW rows found in {SOURCES['SW']}")
        sys.exit(1)

    out = HERE

    # --- latency comparison ------------------------------------------------
    fig, ax = plt.subplots(figsize=(7, 4.5))
    _errorbars(ax, hw, "MATMUL")
    _errorbars(ax, sw, "SW")
    ax.set_ylabel('Elapsed time  (us)')
    ax.set_title('HW Accelerator vs SW Matmul — Latency\n'
                 '(benchmark-app, 100 MHz PL, median +/- min/max, 5 reps)')
    _xaxis(ax)
    fig.tight_layout()
    path = os.path.join(out, "hw_vs_sw_latency.png")
    fig.savefig(path, dpi=150, bbox_inches='tight')
    plt.close(fig)
    print(f"Saved: {path}")

    # --- speedup -----------------------------------------------------------
    common = sorted(set(hw) & set(sw))
    speedups = [sw[N][0] / hw[N][0] for N in common]

    fig, ax = plt.subplots(figsize=(7, 4.5))
    ax.plot(common, speedups, marker='D', color='#9467bd',
            linewidth=2, markersize=7, label='SW / HW speedup')
    ax.fill_between(common, 1.0, speedups, alpha=0.15, color='#9467bd')
    ax.axhline(1.0, color='gray', linestyle='--', linewidth=1,
               label='break-even (1x)')
    for N, s in zip(common, speedups):
        ax.annotate(f'{s:.1f}x', (N, s),
                    textcoords='offset points', xytext=(0, 9),
                    ha='center', fontsize=9)
    ax.set_ylabel('Speedup  (SW time / HW time)')
    ax.set_title('HW Accelerator Speedup over ARM SW Matmul\n'
                 '(100 MHz PL clock)')
    _xaxis(ax)
    fig.tight_layout()
    path = os.path.join(out, "hw_vs_sw_speedup.png")
    fig.savefig(path, dpi=150, bbox_inches='tight')
    plt.close(fig)
    print(f"Saved: {path}")

    # --- summary -----------------------------------------------------------
    print()
    print('=' * 58)
    print('  HW vs SW SUMMARY')
    print('=' * 58)
    print(f"{'N':>5}  {'SW (us)':>12}  {'HW (us)':>12}  {'Speedup':>8}")
    print('-' * 58)
    for N in common:
        print(f"{N:>5}  {sw[N][0]:>12.0f}  {hw[N][0]:>12.0f}  "
              f"{sw[N][0]/hw[N][0]:>7.2f}x")
    print('=' * 58)


if __name__ == '__main__':
    main()
