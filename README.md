# itch-book

A C++20 **Nasdaq TotalView-ITCH 5.0 feed handler and order-book reconstructor**. It replays a
trading day of ITCH messages and rebuilds the order book for every symbol, with correctness
checked against independent implementations and performance reported with bootstrap confidence
intervals.

> It reconstructs books from Nasdaq's outbound feed. It is **not** a matching engine and never
> matches orders itself.

**Status: one real day measured (Nasdaq 12/30/2019, 268,744,780 messages).**
All numbers below come from the files in [docs/results/](docs/results/), run on an Apple M5 (macOS, unpinned). Benchmark numbers are from the second full run, `docs/results/mac-run2/`; the first run is kept in `mac-run1-loaded/` and explained under "What didn't work".

| Headline | Value |
|---|---|
| Day file, message count, symbols | 12302019.NASDAQ_ITCH50, 268,744,780 messages; 8,906 symbols in the stock directory, **8,892 books actually built** (ever non-empty) |
| Throughput, memory-resident, flat store + sorted-vector levels (95% CI, 30 runs) | 9.90 M msgs/s [9.82, 9.97] |
| p50 / p99 / p99.9 per-message latency, same design (95% CI) | 125 ns / 445 ns [432, 459] / 1,113 ns [1,089, 1,140] |
| Baseline (`std::unordered_map` + `std::map`) | 4.06 M msgs/s [3.96, 4.17]; p99 1,004 ns [966, 1,051] |
| Differential mismatches over the day | 0 (1,340 symbol-checkpoints, all four designs) |
| Oracle BBO cross-check (`itch-book` 0.2.1) | 0 mismatches, 5 symbols |

## Architecture

```
mmap'd file  ->  framer (2-byte BE length)  ->  parser (zero-copy decode, switch on type)
             ->  handler (template parameter, no virtual calls)
             ->  OrderStore (order ref -> order)  +  per-symbol Book (price levels, BBO)
             ->  outputs: BBO change log, trade tape, type counts, anomalies, final state hash
```

* `include/itch/` header-only core: `endian.hpp`, `messages.hpp`, `parser.hpp`, `order_store.hpp`
  (two implementations), `book.hpp` (two implementations), `book_handler.hpp`, `reader.hpp`
  (mmap + streaming gzip), `hdr_histogram.hpp`, `timer.hpp`.
* `src/replay.cpp` the replay tool. `tests/` GoogleTest + test-only encoder + synthetic day
  generator. `fuzz/` libFuzzer target. `bench/` Google Benchmark + end-to-end harness.
  `reference/` Python reference book, differential test, oracle check. `analysis/` bootstrap CIs
  and plots. `scripts/` data fetch and benchmark drivers. `docs/` design notes.
* Design rationale, spec deviations from the brief, and the L2-vs-L3 decision: [docs/design.md](docs/design.md).

## Build and run

```
python3 -m venv .venv && .venv/bin/pip install cmake ninja numpy matplotlib   # or use system cmake
cmake -S . -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-rel
ctest --test-dir build-rel --output-on-failure

scripts/fetch_data.sh 12302019 --gunzip      # downloads, gzip -t, records SHA-256
build-rel/replay data/12302019.NASDAQ_ITCH50 --symbols AAPL,MSFT --bbo-out bbo.csv --tape-out tape.csv
```

`replay` prints message counts by type, every anomaly counter, live-order count, printable vs.
non-printable volume, and `state_hash` (identical across runs and across all four designs).
Options: `--store std|flat`, `--levels map|vec`, `--checkpoint-every N --checkpoint-out f`.

## Correctness method

Unit tests (every message type, the edge cases in the brief, all four store x level designs,
invariants verified after every message) | differential test against a slow Python book |
oracle comparison against PyPI `itch-book` | libFuzzer target | ASan/UBSan | determinism check
(two runs, identical hash; designs must agree on the hash).

Results on the real day (files in `docs/results/`):

* Differential test vs. the Python reference: 0 mismatches over 268 checkpoints x 5 symbols
  (1,340 symbol-checkpoints) for all four store x level designs. The test was mutation-checked on
  the synthetic day: a deliberate reference bug produced a mismatch at every checkpoint.
* Oracle: BBO streams, timestamps included, agree for AAPL (424,660 updates), MSFT (600,038),
  BSJR, AHT-F and ISCF.
* All four designs produce the same final state hash `a8c7f832281a39c5`; zero anomaly counters
  except `crossed_after_msg` = 8,649 (books crossed while a symbol is halted or paused, from 15 symbols;
  explained in [docs/design.md](docs/design.md)). 0 live orders remain at end of day.
* 88 unit tests pass in Debug, Release and ASan+UBSan builds; CI (GCC 13 / clang 18 matrix,
  sanitizers, 60 s libFuzzer run, format/tidy, bench smoke) passes on GitHub Actions.
* ASan found one genuine bug during development (use-after-erase in `remove_order`); it is fixed
  and covered by a regression test that fails if the bug is reintroduced.

## Benchmark methodology

Release `-O3`; `scripts/env_report.sh` records CPU, cores, compiler, flags, kernel and governor;
runs are pinned with `taskset` where available (not possible on macOS); warm-up runs excluded;
>= 30 measured runs per design; throughput and latency measured in separate runs; latency by
sampling 1 in N messages with a calibrated tick counter into an HDR-style histogram, with the
measured timer granularity recorded in every row; 95% percentile-bootstrap CIs; a design is only
called an improvement if the CI of the difference excludes zero. Details and caveats in
[docs/design.md](docs/design.md).

```
scripts/run_bench.sh data/12302019.NASDAQ_ITCH50      # full matrix, tables, plot
scripts/reproduce.sh 12302019                         # everything, from a clean checkout
```

## Results

Environment: Apple M5, 10 cores, 24 GB, macOS (Darwin 25.6.0), Apple clang 21, `-O3`, unpinned
(full record in [docs/results/mac-run2/env.txt](docs/results/mac-run2/env.txt)). 30 measured runs per
design after 3 warm-ups, latency sampled 1 in 64 messages. Raw rows:
[docs/results/mac-run2/runs.csv](docs/results/mac-run2/runs.csv). Other applications were running on
the laptop (load average 4.3 at start), so treat absolute numbers as indicative.

| design (store + levels) | throughput (M msgs/s) | p50 ns | p99 ns | p99.9 ns |
|---|---|---|---|---|
| unordered_map + std::map (baseline) | 4.06 [3.96, 4.17] | 175 [170, 181] | 1,004 [966, 1,051] | 1,790 [1,630, 1,982] |
| unordered_map + sorted vector | 5.77 [5.68, 5.85] | 141 [134, 147] | 668 [656, 681] | 1,292 [1,265, 1,323] |
| flat hash + std::map | 7.90 [7.80, 7.99] | 125 [125, 125] | 649 [636, 662] | 1,145 [1,128, 1,163] |
| **flat hash + sorted vector** | **9.90 [9.82, 9.97]** | **125 [125, 125]** | **445 [432, 459]** | **1,113 [1,089, 1,140]** |

Design comparison, flat hash + sorted vector minus baseline (difference, 95% bootstrap CI; an
improvement only if the CI excludes zero):

| metric | difference | CI | verdict |
|---|---|---|---|
| throughput | +5.83 M msgs/s (+143.6%) | [+5.70, +5.96] | improvement |
| p50 | -50.2 ns (-28.7%) | [-57.0, -44.7] | improvement |
| p99 | -559.2 ns (-55.7%) | [-605.7, -518.1] | improvement |
| p99.9 | -677.2 ns (-37.8%) | [-872.8, -515.9] | improvement |

Which change mattered: comparing the means, swapping the order store (baseline to flat hash with
`std::map`) moved throughput by +3.84 M msgs/s, and swapping the price levels (baseline to sorted
vector) by +1.71 M msgs/s; the two together gave +5.83 M. The order-store change is the larger one.
These per-factor differences are read off the table, without their own CIs.

Caveats on this comparison: the four designs were run one after another, not interleaved, so slow
machine drift could contribute to the gaps (the throughput gap is far larger than the run-to-run
spread, but this is not a controlled A/B). The reason the flat designs win is still the data
structure argument (contiguous storage, fewer pointer chases), not a measurement: `perf stat`
counters are unavailable on macOS. The p50 of exactly 125 ns (three ~42 ns timer steps) is quantised,
so those p50 CIs are degenerate and say little beyond "about 125 ns".

Latency histogram for the winning design: ![latency](docs/results/mac-run2/latency-flat-vec.png)

## What didn't work

* The `perf stat` counter attribution and flame graph the brief asks for were not done: they need
  Linux and this was developed on a Mac.
* No x86 / pinned run, so tail latency here is quantised and noisy; treat p99.9 in particular as
  unstable (extreme tail percentiles move a lot between runs).
* The four designs were not interleaved within one session, so the comparison is not a controlled A/B.
* **Throughput gap between tools, resolved.** The first full benchmark run reported 6.46 M msgs/s
  for flat hash + sorted vector, while the single-run `replay` tool reported about 10.5 M for the
  same design. Both tools call the same `consume_frames` loop on the same file (the differences are
  a file sink that is off at runtime in `replay`, prefaulting in `e2e_bench`, and timing only the
  loop). Re-measured on the same day: `replay` gave 10.69 and 9.64 M msgs/s and three
  `e2e_bench` runs gave 9.52, 10.13 and 9.96 M msgs/s, so the tools agree. A second full 30-run
  matrix gave 9.90 M [9.82, 9.97]. The first run (`docs/results/mac-run1-loaded/`) was taken over
  about two hours on a laptop with other processes running (one `std-map` run took about 1,900 s,
  0.14 M msgs/s, and its throughput drifted from 5.1 to 3.7 M early on). I did not isolate which
  process or condition slowed it, so "background load and an unpinned laptop" is the best
  explanation, not a proven one. The first run's headline was dropped; its files are kept.
* Throughput with I/O included (reading from cold disk or the `.gz` stream) was not measured.
* The first version of `remove_order` had a use-after-erase bug, caught by ASan.

## Limitations

* Single real day (12/30/2019); stability across days was not checked (stretch goal, not done).
* Single thread; results are machine-specific.
* On Apple Silicon the timer steps in ~42 ns, so per-message latency is quantised; publish tail
  latency from an x86 Linux host.
* `perf stat` and flame graphs need Linux; not available on the development machine, so no counter-level attribution.
* The synthetic generator can leave books crossed (mid-price drift); real data is expected to
  cross far less, and the observed `crossed_after_msg` count is reported rather than asserted.
* No live-feed concerns (packet loss, gaps, sequence numbers, snapshot recovery) are handled.
* Stretch goals (two-thread SPSC pipeline, multiple days) were not built.
