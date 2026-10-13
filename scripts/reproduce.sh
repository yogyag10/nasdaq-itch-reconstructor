#!/usr/bin/env bash
# One command from a clean checkout to the headline numbers:
#   scripts/reproduce.sh [MMDDYYYY]        (default 12302019; needs ~4 GB download, ~9 GB unpacked)
# Steps: build (Release) -> unit tests -> fetch + unpack the day -> differential test on the
# sampled symbols -> oracle BBO cross-check -> full benchmark matrix with bootstrap CIs.
# Python deps: pip install numpy matplotlib itch-book
set -euo pipefail
cd "$(dirname "$0")/.."
DAY="${1:-12302019}"
SYMS="${SYMS:-AAPL,MSFT}"
EVERY="${EVERY:-100000}"   # checkpoint every N messages for the differential test
REPS="${REPS:-30}"

cmake -S . -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel
ctest --test-dir build-rel --output-on-failure

scripts/fetch_data.sh "$DAY" --gunzip
FILE="data/${DAY}.NASDAQ_ITCH50"

echo "== message counts, anomalies, state hash =="
build-rel/replay "$FILE" | tee "results-replay-$DAY.txt"

echo "== differential test (Python reference is deliberately slow: expect hours on a full day) =="
python3 reference/diff_test.py "$FILE" --replay build-rel/replay --symbols "$SYMS" --every "$EVERY"

echo "== oracle cross-check =="
D="${DAY:4:4}-${DAY:0:2}-${DAY:2:2}"
python3 reference/oracle_check.py "$FILE" --date "$D" --symbols "$SYMS" --replay build-rel/replay

scripts/run_bench.sh "$FILE" "$REPS"
