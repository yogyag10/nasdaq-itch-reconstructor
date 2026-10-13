#!/usr/bin/env python3
"""CI benchmark smoke test: fail on large regressions without any hard-coded machine numbers.

Shared CI runners are noisy and differ by machine, so absolute thresholds would be invented.
Instead, within ONE job on ONE machine we run the baseline design (std OrderStore + std::map) and
the default design (flat OrderStore + flat vector) over the same synthetic day and require:
  1. all runs produce the identical state hash (correctness across designs), and
  2. the default design's median throughput is not more than 20% below the baseline's.
A real regression in the default path (an accidental O(n) or a lost optimisation) trips (2).
"""
import csv
import statistics
import subprocess
import sys

BIN = sys.argv[1] if len(sys.argv) > 1 else "build-rel/e2e_bench"
MSGS = sys.argv[2] if len(sys.argv) > 2 else "1500000"
TOL = 0.80


def run(store, levels):
    out = subprocess.run([BIN, "--synth", MSGS, "--store", store, "--levels", levels, "--reps", "7",
                          "--warmup", "2", "--mode", "throughput"], capture_output=True, text=True, check=True).stdout
    rows = list(csv.DictReader(out.splitlines()))
    return statistics.median(float(r["msgs_per_sec"]) for r in rows), {r["hash"] for r in rows}


base, h1 = run("std", "map")
dflt, h2 = run("flat", "vec")
print(f"baseline std/map median: {base/1e6:.2f} M msgs/s")
print(f"default  flat/vec median: {dflt/1e6:.2f} M msgs/s  (ratio {dflt/base:.2f})")
if len(h1 | h2) != 1:
    sys.exit("FAIL: designs disagree on the final state hash")
if dflt < TOL * base:
    sys.exit(f"FAIL: default design is more than {int((1-TOL)*100)}% slower than baseline")
print("ok")
