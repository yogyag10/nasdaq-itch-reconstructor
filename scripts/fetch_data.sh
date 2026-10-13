#!/usr/bin/env bash
# Download a Nasdaq TotalView-ITCH 5.0 day file, verify it, and record its SHA-256.
#   scripts/fetch_data.sh [MMDDYYYY] [--gunzip]
# Files come from https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/ (check the listing for what is
# currently published; the default is the smallest day seen there). Data is never committed:
# data/ is git-ignored except data/checksums.txt.
#
# Nasdaq does not publish checksums, so verification is: (1) gzip -t integrity, and (2)
# trust-on-first-use: the first download's SHA-256 is recorded in data/checksums.txt and every
# later download of the same file must match it.
set -euo pipefail

DAY="${1:-12302019}"
GUNZIP=0
[[ "${2:-}" == "--gunzip" || "${1:-}" == "--gunzip" ]] && GUNZIP=1
[[ "$DAY" == "--gunzip" ]] && DAY=12302019

cd "$(dirname "$0")/.."
NAME="${DAY}.NASDAQ_ITCH50.gz"
URL="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/${NAME}"
DEST="data/${NAME}"
SUMS="data/checksums.txt"
mkdir -p data
touch "$SUMS"

sha256() { if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }

echo "Downloading $URL (resumable)"
curl -fL -C - --retry 3 -o "$DEST" "$URL"

echo "Checking gzip integrity..."
gzip -t "$DEST"

HASH="$(sha256 "$DEST")"
KNOWN="$(awk -v n="$NAME" '$2==n {print $1}' "$SUMS")"
if [[ -z "$KNOWN" ]]; then
  echo "$HASH  $NAME" >> "$SUMS"
  echo "Recorded SHA-256 (first download): $HASH"
elif [[ "$KNOWN" == "$HASH" ]]; then
  echo "SHA-256 matches recorded value: $HASH"
else
  echo "SHA-256 MISMATCH for $NAME: recorded $KNOWN, got $HASH" >&2
  exit 1
fi

if [[ $GUNZIP -eq 1 ]]; then
  OUT="data/${NAME%.gz}"
  [[ -f "$OUT" ]] || gunzip -k "$DEST"
  echo "Decompressed: $OUT"
fi
