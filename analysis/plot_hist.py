#!/usr/bin/env python3
"""Latency histogram plot from e2e_bench --hist-out (pooled over all measured runs)."""
import argparse
import csv

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import matplotlib.ticker  # noqa: E402,F401
import numpy as np  # noqa: E402

INK, MUTED, GRID, SERIES = "#1f2933", "#52606d", "#e4e7eb", "#2f6fb7"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("hist_csv")
    ap.add_argument("out_png")
    ap.add_argument("--title", default="Per-message latency (sampled), pooled over runs")
    ap.add_argument("--granularity-ns", type=float, default=0.0,
                    help="timer step; shown in the subtitle so quantisation is visible")
    a = ap.parse_args()
    ups, cnt = [], []
    with open(a.hist_csv) as f:
        for r in csv.DictReader(f):
            ups.append(float(r["upper_ns"]))
            cnt.append(float(r["count"]))
    ups, cnt = np.array(ups), np.array(cnt)
    total = cnt.sum()
    cum = np.cumsum(cnt) / total
    pct = {q: ups[np.searchsorted(cum, q)] for q in (0.5, 0.99, 0.999)}
    zero = cnt[ups <= 0].sum() / total  # readings below the timer step cannot sit on a log axis
    keep = ups > 0
    ups, cnt = ups[keep], cnt[keep]

    fig, ax = plt.subplots(figsize=(8, 4.2), dpi=150)
    ax.bar(ups, cnt / total, width=ups * 0.02 + 0.5, color=SERIES, linewidth=0, align="edge")
    ax.set_xscale("log")
    ax.xaxis.set_minor_formatter(matplotlib.ticker.NullFormatter())
    ax.set_yscale("log")
    for q, name in ((0.5, "p50"), (0.99, "p99"), (0.999, "p99.9")):
        ax.axvline(pct[q], color=MUTED, linewidth=1, linestyle=(0, (4, 3)))
        ax.annotate(f"{name} {pct[q]:.0f} ns", (pct[q], 0.97 - 0.08 * (0.5, 0.99, 0.999).index(q)),
                    xycoords=("data", "axes fraction"), xytext=(4, 0), textcoords="offset points",
                    color=INK, fontsize=9, va="top")
    ax.set_xlabel("latency per message (ns, log scale)", color=MUTED)
    ax.set_ylabel("fraction of samples (log scale)", color=MUTED)
    sub = f"{int(total):,} samples" + (f"; timer step {a.granularity_ns:.0f} ns" if a.granularity_ns else "") \
        + (f"; {100*zero:.0f}% read 0 ns (below the step, not drawn)" if zero else "")
    ax.set_title(f"{a.title}\n{sub}", loc="left", color=INK, fontsize=11)
    ax.grid(True, color=GRID, linewidth=0.6)
    ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    for s in ("left", "bottom"):
        ax.spines[s].set_color(GRID)
    ax.tick_params(colors=MUTED)
    fig.tight_layout()
    fig.savefig(a.out_png)
    print(f"wrote {a.out_png}")


if __name__ == "__main__":
    main()
