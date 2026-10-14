#!/usr/bin/env python3
"""Bootstrap confidence intervals over repeated e2e_bench runs.

summary  : per design, mean of each metric across runs with a 95% percentile-bootstrap CI.
compare  : difference B - A of the means with its CI; the verdict is "improvement" only if the
           CI excludes zero in the favourable direction (higher msgs/s, lower latency).

What the CI means: it captures run-to-run variability of the metric (each run = one full replay
= one observation of, e.g., p99). It says nothing about bias from the timer or machine. p99.9
rests on ~1/1000 of the samples in each run, so its run-to-run spread is wide: report it, but
trust the throughput and p50/p99 CIs more.
"""
import argparse
import csv
import sys
from collections import defaultdict

import numpy as np

METRICS = {"msgs_per_sec": +1, "p50_ns": -1, "p99_ns": -1, "p999_ns": -1}  # +1: higher is better
N_BOOT = 10000
SEED = 20240601


def load(paths):
    rows = defaultdict(list)
    for p in paths:
        with open(p, newline="") as f:
            for r in csv.DictReader(f):
                rows[(r["label"], r["mode"])].append(r)
    return rows


STATS = {"mean": np.mean, "median": np.median}


def ci_mean(x, rng, stat="mean"):
    """Percentile-bootstrap CI of the mean (default) or the median over the runs in x."""
    f = STATS[stat]
    x = np.asarray(x, dtype=float)
    idx = rng.integers(0, len(x), size=(N_BOOT, len(x)))
    boots = f(x[idx], axis=1)
    return f(x), *np.percentile(boots, [2.5, 97.5])


def ci_diff(a, b, rng, stat="mean"):
    f = STATS[stat]
    a, b = np.asarray(a, float), np.asarray(b, float)
    ma = f(a[rng.integers(0, len(a), size=(N_BOOT, len(a)))], axis=1)
    mb = f(b[rng.integers(0, len(b), size=(N_BOOT, len(b)))], axis=1)
    d = mb - ma
    return f(b) - f(a), *np.percentile(d, [2.5, 97.5])


def col(rows, m):
    return [float(r[m]) for r in rows]


def cmd_summary(a):
    rng = np.random.default_rng(SEED)
    rows = load(a.csv)
    print(f"| design | mode | runs | metric | {a.stat} | 95% CI |")
    print("|---|---|---|---|---|---|")
    for (label, mode), rs in sorted(rows.items()):
        ms = ["msgs_per_sec"] if mode == "throughput" else ["p50_ns", "p99_ns", "p999_ns"]
        for m in ms:
            mean, lo, hi = ci_mean(col(rs, m), rng, a.stat)
            scale, unit = (1e6, " M") if m == "msgs_per_sec" else (1, "")
            print(f"| {label} | {mode} | {len(rs)} | {m} | {mean/scale:.2f}{unit} | "
                  f"[{lo/scale:.2f}, {hi/scale:.2f}] |")
    for rs in rows.values():
        if len(rs) < 30:
            print(f"\nWARNING: a group has only {len(rs)} runs; the spec asks for >= 30.", file=sys.stderr)
            break


def cmd_compare(a):
    rng = np.random.default_rng(SEED)
    rows = load(a.csv)
    A, B = (a.a, a.mode), (a.b, a.mode)
    for k in (A, B):
        if k not in rows:
            raise SystemExit(f"no rows for design {k[0]!r} mode {k[1]!r}")
    ms = ["msgs_per_sec"] if a.mode == "throughput" else ["p50_ns", "p99_ns", "p999_ns"]
    print(f"{a.b} vs {a.a} ({a.mode}); difference = {a.b} - {a.a}")
    for m in ms:
        d, lo, hi = ci_diff(col(rows[A], m), col(rows[B], m), rng, a.stat)
        base = STATS[a.stat](col(rows[A], m))
        better = METRICS[m]
        excludes = lo > 0 or hi < 0
        if not excludes:
            verdict = "no detectable difference (CI includes 0)"
        elif (d > 0) == (better > 0):
            verdict = "IMPROVEMENT (CI excludes 0)"
        else:
            verdict = "REGRESSION (CI excludes 0)"
        print(f"  {m}: diff {d:+.2f} [{lo:+.2f}, {hi:+.2f}] ({100*d/base:+.1f}% of {a.a}) -> {verdict}")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    sub = ap.add_subparsers(required=True)
    s = sub.add_parser("summary")
    s.add_argument("csv", nargs="+")
    s.add_argument("--stat", choices=list(STATS), default="mean",
                   help="statistic over runs; median resists occasional noisy-host runs")
    s.set_defaults(fn=cmd_summary)
    c = sub.add_parser("compare")
    c.add_argument("csv", nargs="+")
    c.add_argument("--a", required=True, help="baseline design label")
    c.add_argument("--b", required=True, help="candidate design label")
    c.add_argument("--mode", default="throughput", choices=["throughput", "latency"])
    c.add_argument("--stat", choices=list(STATS), default="mean")
    c.set_defaults(fn=cmd_compare)
    args = ap.parse_args()
    args.fn(args)
