#!/usr/bin/env bash
# Run the full benchmark matrix on one day file and print the analysis tables.
#   scripts/run_bench.sh data/12302019.NASDAQ_ITCH50 [REPS=30]
# Designs: {std,flat} OrderStore x {map,vec} price levels. Throughput and latency are separate
# runs because timing inside the loop perturbs the loop. Results land in results/<timestamp>/.
set -euo pipefail
cd "$(dirname "$0")/.."
FILE="${1:?usage: run_bench.sh <decompressed ITCH file> [reps]}"
REPS="${2:-30}"
BIN=build-rel/e2e_bench
[[ -x $BIN ]] || { echo "build first: cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release && cmake --build build-rel"; exit 1; }
OUT="results/$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$OUT"
scripts/env_report.sh | tee "$OUT/env.txt"

PIN=()
if command -v taskset >/dev/null; then PIN=(taskset -c 2); else echo "note: no taskset; runs are not pinned" | tee -a "$OUT/env.txt"; fi

first=1
for store in std flat; do
  for levels in map vec; do
    for mode in throughput latency; do
      HDR=(); [[ $first -eq 0 ]] && HDR=(--no-header)
      EXTRA=(); [[ $mode == latency ]] && EXTRA=(--hist-out "$OUT/hist-$store-$levels.csv")
      # ${arr[@]+"${arr[@]}"}: empty arrays are an "unbound variable" error under `set -u` on bash 3.2 (macOS)
      ${PIN[@]+"${PIN[@]}"} "$BIN" --file "$FILE" --store "$store" --levels "$levels" --mode "$mode" \
        --reps "$REPS" --warmup 3 --csv "$OUT/runs.csv" ${HDR[@]+"${HDR[@]}"} ${EXTRA[@]+"${EXTRA[@]}"}
      first=0
    done
  done
done
echo; echo "== summary (95% bootstrap CIs) =="
python3 analysis/bootstrap.py summary "$OUT/runs.csv" | tee "$OUT/summary.md"
echo; echo "== std-map (baseline) vs flat-vec =="
python3 analysis/bootstrap.py compare "$OUT/runs.csv" --a std-map --b flat-vec --mode throughput
python3 analysis/bootstrap.py compare "$OUT/runs.csv" --a std-map --b flat-vec --mode latency
python3 analysis/plot_hist.py "$OUT/hist-flat-vec.csv" "$OUT/latency-flat-vec.png"
echo "results in $OUT"
