# Interview questions (from the brief) and where to find the evidence

Answer these out loud without notes. The right-hand column is where the code or a measurement
backs the answer, not the answer itself.

| # | Question | Look at |
|---|---|---|
| 1 | Why a reconstructor, not a matching engine? What does ITCH not tell you? | `docs/design.md` top section |
| 2 | Walk an order's life: add, replace, partial execute, cancel | `BookBuilder::on_add/on_replace/on_executed/on_cancel`; `tests/test_book.cpp` |
| 3 | How does C differ from E? What does the printable flag mean for volume? | `on_executed_price`; `ExecutedWithPrice...` test; spec section 1.4.2 |
| 4 | Why fixed-point prices? What breaks with doubles? | `docs/design.md` Prices |
| 5 | Why that OrderStore and price-level structure? Complexity, cache behaviour? | `order_store.hpp`, `book.hpp`, `bench/micro_bench.cpp`, README results |
| 6 | How do you know the book is correct? What could the differential test miss? | `docs/design.md` Correctness machinery |
| 7 | Why p99 and not the mean? Why bootstrap? Why is p99.9 unstable? | `analysis/bootstrap.py` docstring |
| 8 | How did you avoid timer overhead and dead-code elimination? | `bench/e2e_bench.cpp` header, `micro_bench.cpp` (DoNotOptimize), timer granularity column |
| 9 | What changes in a live feed (loss, gaps, sequence numbers, snapshot recovery)? | not built: MoldUDP64 sequence numbers + Glimpse snapshot; see README Limitations |
| 10 | How would you scale to all symbols across machines, and monitor it? | not built; shard by locate, per-shard hash + anomaly counters are the monitoring hooks |
| 11 | (stretch) SPSC queue memory ordering, false sharing | not built |
