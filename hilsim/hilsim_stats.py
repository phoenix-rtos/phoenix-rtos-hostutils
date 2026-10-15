#!/usr/bin/env python3
"""Compute latency statistics and correctness from hilsim dump files (-o).

Usage: hilsim_stats.py <dump> <ideal-dump>

Correctness C is the percentage of motor commands in <dump> equal to the
command at the same position in <ideal-dump> (denominator: ideal command count).
"""

import math
import statistics
import sys

def parse(path):
    """Returns (list of command tuples, list of latencies in us)."""
    cmds = []
    lats = []
    with open(path) as f:
        for line in f:
            tok = line.split()
            if not tok:
                continue
            if len(tok) >= 4 and tok[1] == "cmd":
                # <time> cmd <c0..cn> <latency-us>, only motors sent by craft
                cmds.append(tuple(tok[2:-1]))
                lats.append(int(tok[-1]))
            elif len(tok) >= 4 and tok[1] == "rtt":
                lats.append(int(tok[3]))
    return cmds, lats


def quantile(sorted_vals, q):
    """Linear interpolation between closest ranks (numpy default)."""
    pos = (len(sorted_vals) - 1) * q
    lo = math.floor(pos)
    hi = math.ceil(pos)
    return sorted_vals[lo] + (sorted_vals[hi] - sorted_vals[lo]) * (pos - lo)


def fmt(val, prec=1):
    return f"{val:.{prec}f}".replace(".", ",")


def main():
    if len(sys.argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 1

    cmds, lats = parse(sys.argv[1])
    ideal, _ = parse(sys.argv[2])

    if not lats:
        print("no latency samples in dump", file=sys.stderr)
        return 1

    us = sorted(lats)
    equal = sum(1 for a, b in zip(cmds, ideal) if a == b)
    correct = 100.0 * equal / len(ideal) if ideal else float("nan")

    rows = [
        ("Liczba porcji danych", str(len(us))),
        ("T_max [µs]", fmt(us[-1])),
        ("T_min [µs]", fmt(us[0])),
        ("T_śr [µs]", fmt(statistics.mean(us))),
        ("Mediana [µs]", fmt(statistics.median(us))),
        ("Odch. std. [µs]", fmt(statistics.stdev(us) if len(us) > 1 else 0.0)),
        ("Kwantyl 0,95 [µs]", fmt(quantile(us, 0.95))),
        ("Kwantyl 0,99 [µs]", fmt(quantile(us, 0.99))),
        ("C (poprawność)", f"{fmt(correct, 2)} % ({equal}/{len(ideal)})"),
    ]
    for name, val in rows:
        print(f"{name}\t{val}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
