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
vector) by +1.71 M msgs/s; the two together gave +5.83 M. The order-store change is the larger one on this machine (the x86 run below ranks
them differently). These per-factor differences are read off the table, without their own CIs.

Caveats on this comparison: the four designs were run one after another, not interleaved, so slow
machine drift could contribute to the gaps (the throughput gap is far larger than the run-to-run
spread, but this is not a controlled A/B). The reason the flat designs win is still the data
structure argument (contiguous storage, fewer pointer chases), not a measurement: `perf stat`
counters are unavailable on macOS. The p50 of exactly 125 ns (three ~42 ns timer steps) is quantised,
so those p50 CIs are degenerate and say little beyond "about 125 ns".

Latency histogram for the winning design: ![latency](docs/results/mac-run2/latency-flat-vec.png)

## x86 cross-check (WSL2)

A second machine, run by hand from the same code (`git 5b5f0f2` in `env.txt`, which is commit
`239b6d2` after a later history rewrite) and the same 12/30/2019 file. The Mac results above stay the
headline; this run is a cross-check, not a controlled benchmark. Files:
[docs/results/x86-wsl2-run/](docs/results/x86-wsl2-run/).

Environment: Intel Core i7-1355U laptop CPU, **1 logical core visible to the guest**, 10 GB RAM,
Ubuntu under **WSL2** (Linux 6.18.33.2-microsoft-standard-WSL2), GCC 15.2.0, `-O3`, CPU governor
unavailable, no core pinning possible, load average 0.72 at start. 30 measured runs per design (3
warm-ups), same methodology as the Mac runs.

**Output is identical across machines.** All 240 rows of `runs.csv` (all four designs, both modes)
carry the final state hash `a8c7f832281a39c5` over 268,744,780 messages. That is the same hash as the
Mac `replay` run and the Mac benchmark rows, across arm64 vs x86_64, Apple clang vs GCC, and all four
designs.

Medians with 95% bootstrap CIs (the headline statistic for this run, because several runs were
disturbed by the host, see below):

| design | throughput (M msgs/s) | p50 ns | p99 ns | p99.9 ns |
|---|---|---|---|---|
| unordered_map + std::map (baseline) | 2.87 [2.40, 2.90] | 399 [399, 403] | 1,535 [1,527, 1,559] | 2,575 [2,383, 2,671] |
| unordered_map + sorted vector | 3.95 [3.93, 3.96] | 295 [293, 295] | 975 [971, 979] | 2,423 [2,399, 2,479] |
| flat hash + std::map | 3.91 [3.90, 3.92] | 231 [229, 297] | 1,203 [1,183, 2,527] | 2,551 [2,415, 5,951] |
| **flat hash + sorted vector** | **6.56 [6.54, 6.58]** | **189 [189, 190]** | **575 [575, 579]** | **1,599 [1,547, 1,643]** |

Means with 95% bootstrap CIs (`summary.md`; pulled around by the disturbed runs):

| design | throughput (M msgs/s) | p50 ns | p99 ns | p99.9 ns |
|---|---|---|---|---|
| unordered_map + std::map (baseline) | 2.39 [2.12, 2.64] | 408 [401, 417] | 2,366 [1,748, 3,081] | 4,275 [3,023, 5,712] |
| unordered_map + sorted vector | 3.72 [3.46, 3.93] | 295 [294, 295] | 976 [974, 979] | 2,446 [2,415, 2,481] |
| flat hash + std::map | 3.91 [3.89, 3.92] | 378 [303, 458] | 2,009 [1,631, 2,417] | 9,251 [5,741, 13,030] |
| **flat hash + sorted vector** | **6.56 [6.52, 6.59]** | **189 [189, 190]** | **577 [576, 579]** | **1,592 [1,560, 1,624]** |

Flat hash + sorted vector minus baseline (`compare.txt`): with medians, throughput +3.69 M msgs/s
[+3.65, +4.18], p99 -960 ns [-986, -948]; with means, +4.17 M [+3.91, +4.45] and -1,789 ns
[-2,518, -1,175]. The CIs exclude zero either way, so the direction agrees with the Mac, but the
sizes differ and this machine cannot say how much of the gap is the designs and how much is noise.

Caveats specific to this run:

* **1 vCPU inside a WSL2 VM.** Nothing else is pinned or isolated; the Windows host can steal time
  from the guest at any moment. No governor or turbo control, no `perf` counters were collected and
  no flame graph was made.
* **Disturbed runs.** Baseline throughput: the first run took 269 s (1.00 M msgs/s against a 2.87 M
  median), and 10 of its 30 runs were below 2.4 M. Sorted-vector-with-`unordered_map` had 3 slow
  runs (1.5 to 1.95 M against 3.95 M). Flat hash + `std::map` had a contiguous stretch of runs 14 to
  24 in latency mode with p99.9 up to 29,695 ns against a 2,551 ns median. These look like host
  interference rather than properties of the designs, but I did not isolate the cause. This is why
  the median table is shown first.
* **Timer step.** On three designs the calibrated timer step was about 11 ns (10.7, 11.5 and 12.3
  ns), so p50 is resolved far finer than the Mac's 42 ns step and is not quantised at the scale of
  these differences. The baseline latency pass measured a **75 ns step and a 66 ns back-to-back
  timer floor**, against a 9 to 11 ns floor for the other three, so its p50 (399 ns, about 5 steps) is
  coarser and the baseline latency comparison on this machine is less like-for-like. I did not find
  out why the timer behaved differently in that pass.
* **Ranking differs from the Mac.** By median throughput here the sorted-vector levels (3.95) matter
  about as much as the flat hash store (3.91) on their own, while on the Mac the store change was
  the larger one. I do not have counters to say why.

## What didn't work

* The `perf stat` counter attribution and flame graph the brief asks for were not done. The Mac
  has no `perf`, and the x86 cross-check ran in a WSL2 VM with no counters collected.
* No pinned run on a quiet x86 host. The WSL2 cross-check is unpinned and noisy, and on the Mac
  tail latency is quantised and noisy; treat p99.9 in particular as unstable (extreme tail percentiles move a lot between runs).
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
* **Bug found by the fuzzer: order ref 2^64-1.** One 60 s libFuzzer run in CI trapped on the
  harness's `verify()` invariant check (exit code 77, "deadly signal"). Root cause: `FlatOrderStore`
  marks an empty slot with the key `~0`, so an add whose order reference was exactly 2^64-1 put a
  price level in the book but stored no order, and `find(~0)` matched any empty slot. Reproduced
  locally with a stock directory message plus one add with that ref. Real Nasdaq refs never get
  near that value, so the 268M-message results and state hash were unaffected, but it meant the
  flat store misbehaved on malformed input where the `std` store did not. Fix: `BookBuilder` counts
  an add (or replace target) with that ref as `malformed` and skips the message for every store,
  and `FlatOrderStore` treats `~0` as absent in `insert`, `find` and `erase`. A regression test
  runs the exact input against all four designs. The fuzz harness also used to reuse one handler
  for 256 inputs, which made crashes depend on inputs the fuzzer does not save; it now builds a fresh
  handler per input. The first CI run's crashing input was not saved (no artifact upload then), so
  the reproduction is from the stack trace and the 0xFF-heavy dictionary entries in the log, not
  from the saved file. A single green 60 s fuzz run afterwards is weak evidence on its own; the
  regression test is what pins this case.

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
