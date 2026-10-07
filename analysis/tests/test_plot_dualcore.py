"""
Unit tests for plot_dualcore.py parsing and aggregation logic.

Run with:  pytest analysis/tests/
           pytest analysis/tests/ -v          # verbose
           pytest analysis/tests/ -v -k kv    # run only _kv tests
"""

import sys
import os
import pytest

# Allow importing plot_dualcore from the parent directory
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from plot_dualcore import _kv, parse_lines, aggregate


# ---------------------------------------------------------------------------
# _kv  — key=value field parser
# ---------------------------------------------------------------------------

class TestKv:
    def test_basic(self):
        assert _kv(['L1acc=1000', 'L1miss=5']) == {'L1acc': 1000, 'L1miss': 5}

    def test_non_int_skipped(self):
        # float strings, empty strings, garbage — all silently dropped
        result = _kv(['L1acc=1000', 'L1pct=99.5', 'bad', 'foo=bar'])
        assert result == {'L1acc': 1000}

    def test_empty_list(self):
        assert _kv([]) == {}

    def test_zero_value(self):
        assert _kv(['L1miss=0']) == {'L1miss': 0}

    def test_whitespace_around_equals(self):
        # CSV split + strip leaves no spaces, but be safe
        assert _kv([' L1acc = 500 ']) == {'L1acc': 500}


# ---------------------------------------------------------------------------
# parse_lines  — UART line parser
# ---------------------------------------------------------------------------

GOOD_MATMUL = (
    'MATMUL,64,4587,'
    'L1acc=524189,L1miss=5,L2req=9640,L2hit=4485,L2pct=46'
)
GOOD_SW = (
    'SW_CORE1,64,19121,'
    'L1acc=4789713,L1miss=1756,L1pct=99,'
    'L2req=75849,L2hit=37124,L2pct_approx=48'
)


class TestParseLines:

    # --- happy path ---------------------------------------------------------

    def test_valid_matmul_row(self):
        rows, skipped = parse_lines([GOOD_MATMUL])
        assert skipped == 0
        assert len(rows) == 1
        r = rows[0]
        assert r['mode'] == 'MATMUL'
        assert r['N']    == 64
        assert r['us']   == 4587
        assert r['l1acc'] == 524189
        assert r['l1miss'] == 5
        assert r['l2req']  == 9640
        assert r['l2hit']  == 4485
        assert r['l2pct']  == 46.0

    def test_l1pct_derived_for_matmul(self):
        # MATMUL rows don't carry L1pct in the log; parser derives it
        rows, _ = parse_lines([GOOD_MATMUL])
        r = rows[0]
        expected = (r['l1acc'] - r['l1miss']) / r['l1acc'] * 100
        assert abs(r['l1pct'] - expected) < 0.01

    def test_valid_sw_core1_row(self):
        rows, skipped = parse_lines([GOOD_SW])
        assert skipped == 0
        r = rows[0]
        assert r['mode'] == 'SW_CORE1'
        assert r['N']    == 64
        assert r['us']   == 19121
        assert r['l1pct'] == 99.0
        assert r['l2pct'] == 48.0

    def test_both_modes_in_one_run(self):
        rows, _ = parse_lines([GOOD_MATMUL, GOOD_SW])
        assert len(rows) == 2
        assert {r['mode'] for r in rows} == {'MATMUL', 'SW_CORE1'}

    # --- garbled / invalid lines are skipped --------------------------------

    def test_blank_line_skipped(self):
        _, skipped = parse_lines([''])
        assert skipped == 0  # blank lines don't count as skipped data rows

    def test_comment_line_skipped(self):
        rows, skipped = parse_lines(['# this is a comment'])
        assert rows == []
        assert skipped == 0  # comments are not counted as skipped data rows

    def test_banner_line_skipped(self):
        rows, skipped = parse_lines(['Tiled MatMul: FreeRTOS Dual-Core - Zybo Z7-20'])
        assert rows == []
        assert skipped == 1

    def test_garbled_line_missing_keys(self):
        # Simulates partial UART interleave — required keys are absent
        garbled = 'MATMUL,64,4587,LSW_sOR7=garbage'
        rows, skipped = parse_lines([garbled])
        assert rows == []
        assert skipped == 1

    def test_garbled_matmul_partial_keys(self):
        # Only some keys present — row must be dropped
        partial = 'MATMUL,64,4587,L1acc=524189,L1miss=5'  # missing L2req/hit/pct
        rows, skipped = parse_lines([partial])
        assert rows == []
        assert skipped == 1

    def test_garbled_sw_partial_keys(self):
        partial = 'SW_CORE1,64,19121,L1acc=4789713,L1miss=1756'  # missing L1pct etc.
        rows, skipped = parse_lines([partial])
        assert rows == []
        assert skipped == 1

    def test_unknown_mode_skipped(self):
        rows, skipped = parse_lines(['ACP,64,4587,x=1,y=2,z=3,w=4'])
        assert rows == []
        assert skipped == 1

    def test_n_not_in_allowed_sizes(self):
        # N=100 is not in SIZES = [32, 64, 128, 256, 512]
        line = ('MATMUL,100,4587,'
                'L1acc=524189,L1miss=5,L2req=9640,L2hit=4485,L2pct=46')
        rows, skipped = parse_lines([line])
        assert rows == []
        assert skipped == 1

    def test_negative_us_skipped(self):
        line = ('MATMUL,64,-1,'
                'L1acc=524189,L1miss=5,L2req=9640,L2hit=4485,L2pct=46')
        rows, skipped = parse_lines([line])
        assert rows == []
        assert skipped == 1

    def test_non_numeric_n_skipped(self):
        line = 'MATMUL,abc,4587,L1acc=1,L1miss=0,L2req=1,L2hit=0,L2pct=0'
        rows, skipped = parse_lines([line])
        assert rows == []
        assert skipped == 1

    def test_mixed_good_and_garbled(self):
        lines = [
            GOOD_MATMUL,
            'garbage line that wont parse',
            GOOD_SW,
            'MATMUL,64,4587,broken=no_required_keys',
        ]
        rows, skipped = parse_lines(lines)
        assert len(rows) == 2
        assert skipped == 2

    def test_five_reps_all_parsed(self):
        lines = [GOOD_MATMUL] * 5
        rows, skipped = parse_lines(lines)
        assert len(rows) == 5
        assert skipped == 0


# ---------------------------------------------------------------------------
# aggregate  — median / min / max grouping
# ---------------------------------------------------------------------------

class TestAggregate:

    def _make_rows(self, mode, N, us_values):
        """Build synthetic rows with a list of us values, dummy cache stats."""
        return [
            {'mode': mode, 'N': N, 'us': us,
             'l1acc': 1000, 'l1miss': 10, 'l1pct': 99.0,
             'l2req': 500, 'l2hit': 250, 'l2pct': 50.0}
            for us in us_values
        ]

    def test_single_rep_median_equals_value(self):
        rows = self._make_rows('MATMUL', 64, [4587])
        agg = aggregate(rows)
        us_series = agg['MATMUL']['us']
        assert len(us_series) == 1
        N, med, lo, hi = us_series[0]
        assert N   == 64
        assert med == 4587
        assert lo  == 4587
        assert hi  == 4587

    def test_five_reps_median(self):
        # odd count — median is the middle value
        rows = self._make_rows('MATMUL', 64, [100, 200, 300, 400, 500])
        agg = aggregate(rows)
        _, med, lo, hi = agg['MATMUL']['us'][0]
        assert med == 300
        assert lo  == 100
        assert hi  == 500

    def test_even_reps_median(self):
        # even count — median is average of two middle values
        rows = self._make_rows('SW_CORE1', 128, [100, 200, 300, 400])
        agg = aggregate(rows)
        _, med, lo, hi = agg['SW_CORE1']['us'][0]
        assert med == 250.0
        assert lo  == 100
        assert hi  == 400

    def test_multiple_sizes(self):
        rows  = self._make_rows('MATMUL', 32, [678, 678, 679])
        rows += self._make_rows('MATMUL', 64, [4587, 4588, 4586])
        agg = aggregate(rows)
        Ns = [t[0] for t in agg['MATMUL']['us']]
        assert Ns == [32, 64]   # sorted ascending

    def test_both_modes_independent(self):
        rows  = self._make_rows('MATMUL',   64, [4587])
        rows += self._make_rows('SW_CORE1', 64, [19121])
        agg = aggregate(rows)
        assert 'MATMUL'   in agg
        assert 'SW_CORE1' in agg
        assert agg['MATMUL']['us'][0][1]   == 4587
        assert agg['SW_CORE1']['us'][0][1] == 19121

    def test_missing_mode_not_in_result(self):
        rows = self._make_rows('MATMUL', 64, [4587])
        agg = aggregate(rows)
        assert 'SW_CORE1' not in agg
