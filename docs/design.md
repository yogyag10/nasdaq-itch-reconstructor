# Design notes

## What this is, and what ITCH does not tell you

ITCH is an **outbound** feed. Nasdaq's matching engine decides what trades; ITCH reports the result
(order added, executed, cancelled, deleted, replaced). This project is an **order-book
reconstructor**: it replays those events and never matches anything itself.

ITCH does not tell you: hidden/non-displayed liquidity (only that a trade against it happened, via
`P`), the queue position of anyone but you (we track aggregates), orders from other venues, the
cause of a cancel, or anything before the first message of the file.

## Verified against the official sources

Layouts and lengths were recomputed from the offset tables in Nasdaq's *TotalView-ITCH 5.0*
specification, not copied from the brief. Framing is from Nasdaq's *BinaryFILE* spec: a 2-byte
big-endian length (payload only), then the payload.
([BinaryFILE](https://m.nasdaqtrader.com/content/technicalSupport/specifications/dataproducts/binaryfile.pdf))

Differences from the project brief, all resolved in favour of the spec:

| Item | Brief | Spec |
|---|---|---|
| Length table | listed 8 types | all 23 types are in `messages.hpp`; `tests/test_parser.cpp` pins every one |
| "Others" list | `Y L V W K J h I N` | also `O` (Direct Listing with Capital Raise, 48 bytes) |
| `P` order ref / side | not mentioned | ref is always 0 since Dec 2010; side is always `B` since Jul 2014 |
| `Q` cross trades | "trade tape from E/C/P" | `Q` is counted but not on the tape (a bulk print per symbol, not an order execution) |

## L2 aggregates, not L3 queues

Each side is a collection of price levels holding `{shares, order count}`. We do not keep FIFO
queues because no required output depends on queue position. A replace is remove + add, which is
exactly "priority lost" from the book's point of view. If L3 were ever needed, `Level` would hold
an intrusive list of order indices and `OrderRec` would point into it.

## Prices

`Price` is a `uint32_t` holding Price(4) fixed point. The spec's maximum, 200,000.0000, is
2,000,000,000 (0x77359400) and fits. Doubles cannot represent most decimal prices exactly
(0.1 + 0.2 != 0.3), equality of two prices would stop meaning "same level", and level lookup by
key would silently split one level into two. There is no floating point anywhere in the book.

## Data structures

| Part | Baseline | Alternative | Why the alternative might win |
|---|---|---|---|
| OrderStore | `unordered_map` (node per order, pointer chase) | open addressing, linear probing, backward-shift delete, Fibonacci hash | one contiguous array: a hit is usually one cache line; no per-order allocation |
| Price levels | `std::map` (red-black tree) | sorted flat vector, best at `back()` | activity clusters at the top of book, so insert/erase moves a few elements in cache; `std::map` pays a node allocation and a pointer chase per level |

Complexity: map find/insert/erase O(log L); vector find O(log L) but insert/erase O(L) worst case
(near-O(1) at the top of book, which is where the traffic is). Flat store is O(1) expected at load
factor <= 0.7. Whether the alternatives actually win is a measurement, not an assumption: see
the README results (empty until measured on real data).

The best price is O(1) in both designs by construction (`begin()` / `back()`), so "track the best
bid and offer incrementally" costs nothing extra.

All four combinations must produce a byte-identical state hash; `tests/test_book.cpp` and the
differential test enforce that.

## Correctness machinery

1. **Unit tests** with an independently written test encoder (own big-endian writers, own offsets).
2. **Invariants**: `BookBuilder::verify()` full-scans store vs. levels (no level without orders,
   level totals equal the sum over live orders, no zero-share order). Tests run it after every
   message. Debug builds also trap on a zero-size best level after each message.
3. **Differential test**: a deliberately simple Python book decodes the file itself and top-5
   depth per side is compared at fixed message-index checkpoints. The test fails if it compared
   nothing, and was mutation-checked (dropping partial cancels in the reference produced a
   mismatch at every checkpoint).
4. **Oracle**: `reference/oracle_check.py` compares BBO change streams against the PyPI
   `itch-book` package, using only its public API.
5. **Fuzzing** (libFuzzer) and **ASan/UBSan** in CI. ASan already earned its keep: it caught a
   use-after-erase in `remove_order` that the plain Debug/Release test runs did not flag.

### Anomalies are counted, never fatal

Unknown order references (per message type), duplicate refs, zero-share adds, executes/cancels
larger than the remaining size (clamped), level inconsistencies, malformed and unknown message
types. A message that leaves best bid >= best ask increments `crossed_after_msg` rather than
asserting, because real data can transiently cross or lock (for example around auctions); the
README reports the observed count instead of hiding it.

### What the differential test could miss

* Bugs shared by both implementations (same misreading of the spec). The oracle and the spec-derived
  length table are the mitigation.
* Anything between checkpoints (checkpoint spacing is a trade-off with Python speed).
* Symbols not sampled. Illiquid and liquid names are both in the default list on purpose.
* Only top-5 depth is compared; deeper levels are covered by `verify()` only.
* Real-data quirks absent from the synthetic generator. The synthetic day proves the machinery;
  the real day is the evidence.

## Measurement notes

* Throughput runs have no timers inside the loop. Latency runs time **1 in N messages** (default
  N=64) with the hardware tick counter, or whole batches, because timing every message would add
  more overhead than the work being measured.
* Every latency row carries the **measured timer granularity**. On the Apple Silicon laptop this
  was developed on, the counter is nominally 1 GHz but advances in ~42 ns steps, so per-message
  percentiles there are quantised and mostly meaningful as "multiples of 42 ns". Publish tail
  latency from an x86 host (`rdtscp`) or use batch mode.
* The CI "benchmark smoke" check is **relative** (default design vs. baseline within one job),
  because absolute thresholds on shared runners would be invented numbers.
* Bootstrap CIs (10,000 resamples, percentile method) are over run-to-run variation. p99.9 rests
  on ~0.1% of each run's samples, so it is the least stable statistic; report it but weight p50,
  p99 and throughput more.
* The synthetic-file throughput seen in smoke runs fits in cache and says nothing about a real day.
