# itch-book

A C++20 **Nasdaq TotalView-ITCH 5.0 feed handler and order-book reconstructor**. It replays a
trading day of ITCH messages and rebuilds the order book for every symbol, with correctness
checked against independent implementations and performance reported with bootstrap confidence
intervals.

> It reconstructs books from Nasdaq's outbound feed. It is **not** a matching engine and never
> matches orders itself.

**Status: harness complete; real-day results not yet measured.** The headline slots below are
deliberately empty rather than estimated. Everything marked "verified" was run, and on synthetic
data only (see Limitations).

| Headline | Value |
|---|---|
| Day file, message count, symbols | _not yet run_ |
| Throughput, memory-resident (95% CI) | _not yet run_ |
| p50 / p99 / p99.9 per-message latency (95% CI) | _not yet run_ |
| Differential mismatches over the day | _not yet run_ |
| Oracle BBO cross-check | _not yet run on a real day_ |

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

Verified so far (synthetic seeded day, not real data):

* 88 unit tests pass in Debug, Release and ASan+UBSan builds.
* Differential test: 0 mismatches for all four designs on a 60k-message synthetic day
  (948 symbol-checkpoints). The test was mutation-checked: a deliberate reference bug produced a
  mismatch at every checkpoint.
* Oracle (`itch-book` 0.2.1) BBO streams, timestamps included, agree on the synthetic day for 3 symbols.
* Fuzz corpus of 400 mutated inputs replays cleanly under ASan/UBSan (the real libFuzzer run
  happens in CI on Linux; Apple clang ships no libFuzzer).
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

_Not yet measured on a real day file. Tables (throughput, latency percentiles with CIs, the
std::map vs. flat-vector and unordered_map vs. flat-hash comparisons with `perf stat` counters)
and the latency histogram will be added here from `results/<timestamp>/`, and only from those
files._

## What didn't work

_To be filled in during measurement._ Logged so far: nothing yet beyond the ASan finding above.

## Limitations

* Everything verified so far used a **synthetic** day built with the test encoder; the real-day
  differential, oracle and benchmark runs have not been done yet.
* Single day, single thread, results are machine-specific.
* On Apple Silicon the timer steps in ~42 ns, so per-message latency is quantised; publish tail
  latency from an x86 Linux host.
* `perf stat` and flame graphs need Linux; they are not available on the development machine.
* The synthetic generator can leave books crossed (mid-price drift); real data is expected to
  cross far less, and the observed `crossed_after_msg` count is reported rather than asserted.
* The GitHub Actions workflow has not run yet. Local verification was on macOS with Apple clang
  only; the GCC 13 / clang 18 Linux matrix, the real libFuzzer job and the pinned format/tidy job
  are unconfirmed until the first push builds.
* No live-feed concerns (packet loss, gaps, sequence numbers, snapshot recovery) are handled.
