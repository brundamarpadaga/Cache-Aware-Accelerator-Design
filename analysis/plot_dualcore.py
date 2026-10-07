#!/usr/bin/env python3
"""
plot_dualcore.py  —  Dual-core AMP benchmark plotter
=====================================================
Board:  Digilent Zybo Z7-20 (XC7Z020-1CLG400C)
Branch: feature/freertos-dual-core

Parses the key=value UART output produced by both cores running
simultaneously. Garbled lines (interleaved UART characters before the
spinlock fix) are silently skipped when required fields are missing.

MATMUL row (Core 0 — HLS accelerator, FreeRTOS):
  MATMUL,N,us,L1acc=...,L1miss=...,L2req=...,L2hit=...,L2pct=...

SW_CORE1 row (Core 1 — bare-metal ARM i-k-j loop):
  SW_CORE1,N,us,L1acc=...,L1miss=...,L1pct=...,L2req=...,L2hit=...,L2pct_approx=...

Usage:
  python3 plot_dualcore.py results_dualcore.txt --out figures_dualcore/
  python3 plot_dualcore.py results_a.txt results_b.txt --out figures/
  python3 plot_dualcore.py -               # read from stdin
  python3 plot_dualcore.py results.txt     # print summary only (no --out)
"""

import sys
import argparse
import os
from collections import defaultdict
from statistics import median

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import matplotlib.ticker as ticker
    HAS_MPL = True
except ImportError:
    HAS_MPL = False
    print("WARNING: matplotlib not found  ->  pip install matplotlib")

SIZES   = [32, 64, 128, 256, 512]
COLORS  = {"MATMUL": "#9467bd", "SW_CORE1": "#2ca02c"}
MARKERS = {"MATMUL": "D",       "SW_CORE1": "^"}
LABELS  = {"MATMUL":  "HW  (matmul_0, Core 0, FreeRTOS)",
           "SW_CORE1":"SW  (ARM i-k-j, Core 1, bare-metal)"}


# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------

def _kv(fields):
    """Parse 'key=value' list into a dict of int values; non-int values are skipped."""
    out = {}
    for f in fields:
        if '=' not in f:
            continue
        k, _, v = f.partition('=')
        try:
            out[k.strip()] = int(v.strip())
        except ValueError:
            pass
    return out


def parse_lines(lines):
    """
    Parse an iterable of UART log lines into a list of row dicts.
    Garbled / non-data lines are silently skipped.
    Returns (rows, skipped_count).
    """
    rows, skipped = [], 0
    for line in lines:
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        parts = [p.strip() for p in line.split(',')]
        if len(parts) < 4:
            skipped += 1
            continue

        mode = parts[0]
        if mode not in ('MATMUL', 'SW_CORE1'):
            skipped += 1
            continue

        try:
            N  = int(parts[1])
            us = int(parts[2])
        except ValueError:
            skipped += 1
            continue

        if N not in SIZES or us <= 0:
            skipped += 1
            continue

        kv = _kv(parts[3:])

        if mode == 'MATMUL':
            required = ('L1acc', 'L1miss', 'L2req', 'L2hit', 'L2pct')
            if not all(k in kv for k in required):
                skipped += 1
                continue
            l1acc  = kv['L1acc']
            l1miss = kv['L1miss']
            l1pct  = (l1acc - l1miss) / l1acc * 100 if l1acc > 0 else 0.0
            l2pct  = float(kv['L2pct'])
            l2req  = kv['L2req']
            l2hit  = kv['L2hit']
        else:  # SW_CORE1
            required = ('L1acc', 'L1miss', 'L1pct', 'L2req', 'L2hit', 'L2pct_approx')
            if not all(k in kv for k in required):
                skipped += 1
                continue
            l1acc  = kv['L1acc']
            l1miss = kv['L1miss']
            l1pct  = float(kv['L1pct'])
            l2pct  = float(kv['L2pct_approx'])
            l2req  = kv['L2req']
            l2hit  = kv['L2hit']

        rows.append({
            'mode': mode, 'N': N, 'us': us,
            'l1acc': l1acc, 'l1miss': l1miss, 'l1pct': l1pct,
            'l2req': l2req, 'l2hit': l2hit, 'l2pct': l2pct,
        })

    return rows, skipped


def parse_log(source):
    """Open a UART log file (or '-' for stdin) and call parse_lines."""
    if source == '-':
        lines = sys.stdin.readlines()
    else:
        with open(source, errors='replace') as fh:
            lines = fh.readlines()

    rows, skipped = parse_lines(lines)
    src_label = source if source != '-' else 'stdin'
    print(f"  {src_label}: {len(rows)} rows parsed, {skipped} lines skipped")
    return rows


# ---------------------------------------------------------------------------
# Aggregation
# ---------------------------------------------------------------------------

def aggregate(rows):
    """
    Returns agg[mode][field] = list of (N, median, min, max), sorted by N.
    Only field lists with at least one value are included.
    """
    buckets = defaultdict(lambda: defaultdict(lambda: defaultdict(list)))
    for r in rows:
        for field in ('us', 'l1acc', 'l1miss', 'l1pct', 'l2req', 'l2hit', 'l2pct'):
            buckets[r['mode']][r['N']][field].append(r[field])

    agg = {}
    for mode in ('MATMUL', 'SW_CORE1'):
        if mode not in buckets:
            continue
        agg[mode] = defaultdict(list)
        for N in sorted(buckets[mode]):
            for field in ('us', 'l1acc', 'l1miss', 'l1pct', 'l2req', 'l2hit', 'l2pct'):
                vals = buckets[mode][N][field]
                if vals:
                    agg[mode][field].append(
                        (N, median(vals), min(vals), max(vals))
                    )
    return agg


# ---------------------------------------------------------------------------
# Plotting helpers
# ---------------------------------------------------------------------------

def _series(agg, mode, field):
    data = agg.get(mode, {}).get(field, [])
    if not data:
        return [], [], [], []
    Ns   = [t[0] for t in data]
    meds = [t[1] for t in data]
    los  = [t[2] for t in data]
    his  = [t[3] for t in data]
    return Ns, meds, los, his


def _plot_mode(ax, agg, mode, field, *, label_override=None):
    Ns, meds, los, his = _series(agg, mode, field)
    if not Ns:
        return
    label  = label_override or LABELS[mode]
    lo_err = [m - lo for m, lo in zip(meds, los)]
    hi_err = [hi - m  for m, hi in zip(meds, his)]
    ax.errorbar(Ns, meds, yerr=[lo_err, hi_err],
                label=label, color=COLORS[mode], marker=MARKERS[mode],
                markersize=6, linewidth=1.8, capsize=4, elinewidth=1.2)
    ax.fill_between(Ns, los, his, color=COLORS[mode], alpha=0.12)


def _xaxis(ax):
    ax.set_xscale('log', base=2)
    ax.set_xticks(SIZES)
    ax.xaxis.set_major_formatter(ticker.ScalarFormatter())
    ax.set_xlabel('Matrix side  N')
    ax.grid(True, which='both', linestyle='--', alpha=0.4)
    ax.legend(framealpha=0.9)


def _save(fig, out_dir, name):
    path = os.path.join(out_dir, name)
    fig.savefig(path, dpi=150, bbox_inches='tight')
    plt.close(fig)
    print(f"  Saved: {path}")


# ---------------------------------------------------------------------------
# Figures
# ---------------------------------------------------------------------------

def plot_latency(agg, out_dir):
    fig, ax = plt.subplots(figsize=(7, 4.5))
    for mode in ('MATMUL', 'SW_CORE1'):
        _plot_mode(ax, agg, mode, 'us')
    ax.set_ylabel('Elapsed time  (µs)')
    ax.set_title('HW Accelerator vs SW Matmul — Latency\n'
                 '(both cores concurrent, median ± min/max, 5 reps)')
    _xaxis(ax)
    fig.tight_layout()
    _save(fig, out_dir, 'latency.png')


def plot_speedup(agg, out_dir):
    hw_us = {t[0]: t[1] for t in agg.get('MATMUL',   {}).get('us', [])}
    sw_us = {t[0]: t[1] for t in agg.get('SW_CORE1', {}).get('us', [])}
    common = sorted(set(hw_us) & set(sw_us))
    if not common:
        print("  Skipping speedup plot — need both MATMUL and SW_CORE1 rows")
        return
    speedups = [sw_us[N] / hw_us[N] for N in common]

    fig, ax = plt.subplots(figsize=(7, 4.5))
    ax.plot(common, speedups, marker='D', color='#9467bd',
            linewidth=2, markersize=7, label='SW / HW speedup')
    ax.fill_between(common, 1.0, speedups, alpha=0.15, color='#9467bd')
    ax.axhline(1.0, color='gray', linestyle='--', linewidth=1, label='break-even (1×)')
    for N, s in zip(common, speedups):
        ax.annotate(f'{s:.2f}×', (N, s),
                    textcoords='offset points', xytext=(0, 9),
                    ha='center', fontsize=9)
    ax.set_ylabel('Speedup  (SW time / HW time)')
    ax.set_title('HW Accelerator Speedup over ARM SW Matmul\n'
                 '(concurrent dual-core run)')
    _xaxis(ax)
    fig.tight_layout()
    _save(fig, out_dir, 'speedup.png')


def plot_l1_hit(agg, out_dir):
    fig, ax = plt.subplots(figsize=(7, 4.5))
    for mode in ('MATMUL', 'SW_CORE1'):
        _plot_mode(ax, agg, mode, 'l1pct')
    ax.set_ylabel('L1 D-cache hit rate  (%)')
    ax.set_ylim(-5, 110)
    ax.set_title('L1 D-Cache Hit Rate — Per Core\n'
                 '(ARM CP15 PMU event 0x03/0x04, private per core — accurate)')
    _xaxis(ax)
    ax.annotate(
        'Core 0 (HW): near-zero L1 accesses confirm Core 0 is\n'
        'idle during accelerator DMA — not an L1 measurement',
        xy=(0.02, 0.04), xycoords='axes fraction',
        fontsize=7.5, color=COLORS['MATMUL'],
        bbox=dict(boxstyle='round,pad=0.3', fc='white', alpha=0.7))
    fig.tight_layout()
    _save(fig, out_dir, 'l1_hit_rate.png')


def plot_l1_misses(agg, out_dir):
    fig, ax = plt.subplots(figsize=(7, 4.5))
    for mode in ('MATMUL', 'SW_CORE1'):
        _plot_mode(ax, agg, mode, 'l1miss')
    ax.set_ylabel('L1 D-cache miss count')
    ax.set_title('L1 D-Cache Miss Count — Per Core\n'
                 '(ARM CP15 PMU event 0x03)')
    _xaxis(ax)
    fig.tight_layout()
    _save(fig, out_dir, 'l1_misses.png')


def plot_l2_hit(agg, out_dir):
    fig, ax = plt.subplots(figsize=(7, 4.5))
    for mode in ('MATMUL', 'SW_CORE1'):
        _plot_mode(ax, agg, mode, 'l2pct')
    ax.set_ylabel('L2 hit rate  (%)  [PL310  DRHIT / DRREQ]')
    ax.set_ylim(-5, 110)
    ax.set_title('L2 Cache Hit Rate — Shared PL310 Counter\n'
                 '(aggregate traffic: both cores + ACP — values are approximate)')
    _xaxis(ax)
    ax.annotate(
        '⚠  PL310 counters are shared: both cores\' and ACP traffic\n'
        'are mixed into one reading — not per-core accurate',
        xy=(0.02, 0.04), xycoords='axes fraction',
        fontsize=8, color='#d62728',
        bbox=dict(boxstyle='round,pad=0.3', fc='white', alpha=0.7))
    fig.tight_layout()
    _save(fig, out_dir, 'l2_hit_rate.png')


# ---------------------------------------------------------------------------
# Text summary
# ---------------------------------------------------------------------------

def print_summary(agg):
    def _med(mode, field, N):
        return next((t[1] for t in agg.get(mode, {}).get(field, []) if t[0] == N),
                    float('nan'))

    print()
    print('=' * 72)
    print('  DUAL-CORE BENCHMARK SUMMARY  (median of reps)')
    print('=' * 72)
    print(f"{'Mode':<12} {'N':>5} {'us':>14} {'L1 hit%':>9} {'L2 hit%':>9}")
    print('-' * 72)
    for mode in ('MATMUL', 'SW_CORE1'):
        if mode not in agg:
            continue
        for t in agg[mode].get('us', []):
            N, med_us = t[0], t[1]
            l1 = _med(mode, 'l1pct', N)
            l2 = _med(mode, 'l2pct', N)
            print(f"{mode:<12} {N:>5} {med_us:>14.0f} {l1:>9.1f} {l2:>9.1f}")
        print()

    hw = {t[0]: t[1] for t in agg.get('MATMUL',   {}).get('us', [])}
    sw = {t[0]: t[1] for t in agg.get('SW_CORE1', {}).get('us', [])}
    common = sorted(set(hw) & set(sw))
    if common:
        print('-' * 72)
        print(f"{'':12} {'N':>5} {'speedup':>14}   (SW us / HW us)")
        print('-' * 72)
        for N in common:
            print(f"{'':12} {N:>5} {sw[N]/hw[N]:>14.2f}x")
        print()
    print('=' * 72)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description='Plot dual-core AMP benchmark results from Zynq UART log.')
    ap.add_argument('sources', nargs='*', default=['-'],
                    help="UART log file(s) or '-' for stdin")
    ap.add_argument('--out', '-o', metavar='DIR', default=None,
                    help='Existing directory to save PNG figures; '
                         'defaults to the directory of the first source file '
                         '(or cwd for stdin). Omit to print summary only.')
    args = ap.parse_args()

    print(f"Parsing {len(args.sources)} source(s)...")
    rows = []
    for src in args.sources:
        rows.extend(parse_log(src))

    if not rows:
        print('\nERROR: No valid MATMUL or SW_CORE1 rows found.')
        print('Expected format:')
        print('  MATMUL,64,4584,L1acc=524189,L1miss=5,L2req=9640,L2hit=4485,L2pct=46')
        print('  SW_CORE1,64,19122,L1acc=4789713,L1miss=1756,L1pct=99,'
              'L2req=75849,L2hit=37124,L2pct_approx=48')
        sys.exit(1)

    agg = aggregate(rows)
    print_summary(agg)

    if args.out is None:
        # Default: figures go next to the first source file (not for stdin)
        first = args.sources[0]
        if first == '-':
            return  # stdin + no --out: summary only
        args.out = os.path.dirname(os.path.abspath(first))
        print(f'\n--out not specified, using {args.out}')

    if not os.path.isdir(args.out):
        print(f'ERROR: output directory does not exist: {args.out}')
        print('Create it first or pass an existing directory with --out.')
        sys.exit(1)

    if not HAS_MPL:
        print('ERROR: matplotlib required for plots — pip install matplotlib')
        sys.exit(1)

    print(f'\nGenerating plots -> {args.out}')
    plot_latency (agg, args.out)
    plot_speedup (agg, args.out)
    plot_l1_hit  (agg, args.out)
    plot_l1_misses(agg, args.out)
    plot_l2_hit  (agg, args.out)
    print('Done.')


if __name__ == '__main__':
    main()
