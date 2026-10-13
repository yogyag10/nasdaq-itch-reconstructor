#!/usr/bin/env python3
"""Cross-check BBO against an independent open-source implementation (PyPI `itch-book`).

Used purely as a comparison oracle: we only call its public Feed API and never reuse its code
or design. Both sides produce a BBO change stream per symbol; we drop consecutive duplicates
(the oracle may also emit on order-count changes, which we do not log) and require the
sequences of (ts, bid_px, bid_sz, ask_px, ask_sz) to be identical. Prices compared as integers.

    pip install itch-book
    oracle_check.py FILE --date 2019-12-30 --symbols AAPL,MSFT
"""
import argparse
import datetime as dt
import os
import subprocess
import sys
import tempfile

import numpy as np


def ours(replay, path, symbols, store, levels):
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "bbo.csv")
        r = subprocess.run([replay, path, "--symbols", ",".join(symbols), "--bbo-out", out,
                            "--store", store, "--levels", levels, "--no-timing"],
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise SystemExit(r.stderr or r.stdout)
        res = {s: [] for s in symbols}
        with open(out) as f:
            for ln in f:
                ts, sym, bp, bs, ap_, as_ = ln.strip().split(",")
                res[sym].append((int(ts), int(bp), int(bs), int(ap_), int(as_)))
        return res


def theirs(path, date, symbols):
    from itch_book import Feed
    from itch_book._dates import midnight_ns

    mid = int(midnight_ns(date))
    cols = {k: [] for k in ("ts_event", "locate", "bid_px", "bid_sz", "ask_px", "ask_sz")}
    sym_of = {}
    feed = Feed(path, date=date, price_type="fixed")
    with feed:
        for b in feed.batches(tables=("bbo",), symbols=tuple(symbols)):
            for k in cols:
                cols[k].append(np.asarray(b.bbo[k]))
    # Symbol names per locate come from the stock directory; take them from our own run's file
    # order instead of the oracle's symbol table to keep the two sides decoupled.
    return mid, {k: np.concatenate(v) if v else np.array([]) for k, v in cols.items()}


def dedup(rows):
    out = []
    for r in rows:
        if not out or out[-1][1:] != r[1:]:
            out.append(r)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--date", required=True, type=dt.date.fromisoformat)
    ap.add_argument("--symbols", default="AAPL,MSFT")
    ap.add_argument("--replay", default="build/replay")
    ap.add_argument("--store", default="flat")
    ap.add_argument("--levels", default="vec")
    ap.add_argument("--no-ts", action="store_true", help="compare states only, not timestamps")
    a = ap.parse_args()
    syms = [s for s in a.symbols.split(",") if s]
    mine = ours(a.replay, a.file, syms, a.store, a.levels)

    # Map locate -> symbol via one extra replay-independent pass over R messages.
    sys.path.insert(0, os.path.dirname(__file__))
    import ref_book
    import gzip
    opener = gzip.open if a.file.endswith(".gz") else open
    loc_of = {}
    with opener(a.file, "rb") as f:
        for m in ref_book.frames(f):
            if m[:1] == b"R":
                s = m[11:19].decode().rstrip(" ")
                if s in syms:
                    loc_of[s] = int.from_bytes(m[1:3], "big")
            if len(loc_of) == len(syms):
                break

    mid, t = theirs(a.file, a.date, syms)
    bad = 0
    for s in syms:
        if s not in loc_of:
            print(f"{s}: not in stock directory")
            bad += 1
            continue
        sel = t["locate"] == loc_of[s]
        theirs_rows = [(int(ts) - mid, int(bp), int(bs), int(ap_), int(as_)) for ts, bp, bs, ap_, as_ in
                       zip(t["ts_event"][sel], t["bid_px"][sel], t["bid_sz"][sel],
                           t["ask_px"][sel], t["ask_sz"][sel])]
        x, y = dedup(mine[s]), dedup(theirs_rows)
        if a.no_ts:
            x, y = [r[1:] for r in dedup([(0,) + r[1:] for r in x])], [r[1:] for r in dedup([(0,) + r[1:] for r in y])]
        n = sum(1 for p, q in zip(x, y) if p != q) + abs(len(x) - len(y))
        print(f"{s}: ours={len(x)} oracle={len(y)} mismatches={n}")
        if n:
            for i, (p, q) in enumerate(zip(x, y)):
                if p != q:
                    print(f"  first diff at #{i}: ours={p} oracle={q}")
                    break
        bad += n
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
