#!/usr/bin/env python3
"""Compute latency statistics and correctness from hilsim dump CSV files (-o).

Usage: hilsim_stats.py <dump> <ideal-dump>

Correctness C is the percentage of motor commands in <ideal-dump> for which
<dump> has a command with the same idx (scenario row) and equal value
(denominator: ideal command count, a missing idx counts as incorrect).
"""

import csv
import math
import statistics
import sys

def parse(path):
    """Returns (dict idx -> command tuple, list of latencies in us)."""
    cmds = {}
    lats = []
    with open(path, newline="") as f:
        # '#' lines are run metadata (start time, input file)
        reader = csv.DictReader(line for line in f if not line.startswith("#"))
        cmd_cols = [c for c in (reader.fieldnames or []) if c.startswith("cmd")]
        for row in reader:
            # commands: idx,sent_ns,recv_ns,cmd0..cmdN, echoes (-p): idx,sent_ns,recv_ns,seq
            cmds[int(row["idx"])] = tuple(row[c] for c in cmd_cols)
            if "latency_us" in row:
                lats.append(int(row["latency_us"]))
            else:
                lats.append((int(row["recv_ns"]) - int(row["sent_ns"])) / 1000)
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
    equal = sum(1 for idx, cmd in ideal.items() if cmds.get(idx) == cmd)
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
