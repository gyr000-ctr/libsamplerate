#!/usr/bin/env python3
"""
Bit-level regression gate for the float (default) precision path.

Compares a hex-float dump produced by the modified float build against a
reference dump produced from the pristine tree (commit 0844c20). Every
output sample must match bit for bit; any difference fails the gate.

The precision header line ("# precision: ...") is ignored; only CASE
headers and sample lines are compared, in order.

Run isolated:
    python3 -I bit_regression_check.py baseline_dump.txt candidate_dump.txt
"""

import sys


def load_body(path):
    lines = []
    with open(path) as fh:
        for line in fh:
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            lines.append(s)
    return lines


def main():
    baseline, candidate = sys.argv[1], sys.argv[2]
    a = load_body(baseline)
    b = load_body(candidate)
    if len(a) != len(b):
        print("FAIL: line count differs (%d vs %d)" % (len(a), len(b)))
        return 1
    diffs = [(i, x, y) for i, (x, y) in enumerate(zip(a, b)) if x != y]
    if diffs:
        print("FAIL: %d lines differ, first differences:" % len(diffs))
        for i, x, y in diffs[:10]:
            print("  line %d: baseline=%s candidate=%s" % (i + 1, x, y))
        return 1
    print("PASS: float path bit-identical to pristine baseline (%d sample/case lines)" % len(a))
    return 0


if __name__ == "__main__":
    sys.exit(main())
