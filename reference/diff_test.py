#!/usr/bin/env python3
"""Differential test: C++ replay vs the Python reference book on the same file.

Compares top-5 levels per side for the chosen symbols at every checkpoint. Exit code 0 only if
there are zero mismatches AND at least one checkpoint was compared (an empty comparison is a
failure, not a pass).
"""
import argparse
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(__file__))
import ref_book  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--replay", default="build/replay")
    ap.add_argument("--symbols", default="AAPL,MSFT,ILQ102,ILQ103")
    ap.add_argument("--every", type=int, default=1000)
    ap.add_argument("--store", default="flat")
    ap.add_argument("--levels", default="vec")
    ap.add_argument("--show", type=int, default=5, help="print first N mismatches")
    a = ap.parse_args()

    with tempfile.TemporaryDirectory() as d:
        cpp_out = os.path.join(d, "cpp.txt")
        ref_out = os.path.join(d, "ref.txt")
        r = subprocess.run(
            [a.replay, a.file, "--symbols", a.symbols, "--checkpoint-every", str(a.every),
             "--checkpoint-out", cpp_out, "--store", a.store, "--levels", a.levels, "--no-timing"],
            capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stderr or r.stdout, file=sys.stderr)
            return 2
        with open(ref_out, "w") as out:
            n, _ = ref_book.run(a.file, a.symbols.split(","), a.every, out)
        with open(cpp_out) as f1, open(ref_out) as f2:
            cpp, ref = f1.read().splitlines(), f2.read().splitlines()

    mism = 0
    for i, (x, y) in enumerate(zip(cpp, ref)):
        if x != y:
            mism += 1
            if mism <= a.show:
                print(f"MISMATCH line {i}:\n  cpp: {x}\n  ref: {y}")
    if len(cpp) != len(ref):
        mism += abs(len(cpp) - len(ref))
        print(f"line count differs: cpp={len(cpp)} ref={len(ref)}")
    checkpoints = len({ln.split()[0] for ln in ref})
    print(f"messages={n} checkpoints={checkpoints} symbol-checkpoints={len(ref)} "
          f"mismatches={mism} design={a.store}/{a.levels}")
    if not ref:
        print("FAIL: nothing was compared (symbols not in file?)")
        return 1
    return 0 if mism == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
