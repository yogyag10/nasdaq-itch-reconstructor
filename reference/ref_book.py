#!/usr/bin/env python3
"""Deliberately simple, slow reference order book for the differential test.

Written independently of the C++ code: its own struct-based decoder, plain dicts for orders
and price levels, sorted() for depth. It tracks only the symbols you ask for and prints the
same checkpoint format as `replay --checkpoint-out`:

    <msg_index> <SYMBOL> B:px@shares/orders,... A:px@shares/orders,...

msg_index counts every BinaryFILE frame the C++ side counts (zero-length frames are skipped,
unknown or wrong-length frames are counted but ignored). Prices stay integers (4 implied
decimals) throughout.
"""
import argparse
import gzip
import struct
import sys

# Total payload length per message type, from the spec offset tables (independent copy).
LENGTHS = {
    "S": 12, "R": 39, "H": 25, "Y": 20, "L": 26, "V": 35, "W": 12, "K": 28, "J": 35, "h": 21,
    "A": 36, "F": 40, "E": 31, "C": 36, "X": 23, "D": 19, "U": 35, "P": 44, "Q": 40, "B": 19,
    "I": 50, "N": 20, "O": 48,
}


class RefBook:
    def __init__(self, symbols):
        self.wanted = set(symbols)
        self.locate_to_sym = {}  # selected locates only
        self.orders = {}  # ref -> [locate, side, price, shares]
        self.levels = {}  # (locate, side) -> {price: [shares, orders]}

    def _level(self, loc, side):
        return self.levels.setdefault((loc, side), {})

    def _add(self, loc, side, price, shares):
        lv = self._level(loc, side).setdefault(price, [0, 0])
        lv[0] += shares
        lv[1] += 1

    def _reduce(self, loc, side, price, shares, order_gone):
        d = self._level(loc, side)
        lv = d[price]
        lv[0] -= shares
        if order_gone:
            lv[1] -= 1
        if lv[1] == 0:
            del d[price]

    def handle(self, m):
        t = chr(m[0])
        loc = struct.unpack_from(">H", m, 1)[0]
        if t == "R":
            sym = m[11:19].decode("ascii").rstrip(" ")
            if sym in self.wanted:
                self.locate_to_sym[loc] = sym
            return
        if loc not in self.locate_to_sym:
            return
        if t in "AF":
            ref, side, shares = struct.unpack_from(">QcI", m, 11)
            price = struct.unpack_from(">I", m, 32)[0]
            if shares == 0 or ref in self.orders:
                return
            side = side.decode()
            if side not in "BS":
                return
            self.orders[ref] = [loc, side, price, shares]
            self._add(loc, side, price, shares)
        elif t in "EC":
            ref, shares = struct.unpack_from(">QI", m, 11)
            if t == "C" and m[31:32] not in (b"Y", b"N"):
                return
            o = self.orders.get(ref)
            if o is None:
                return
            n = min(shares, o[3])
            gone = n == o[3]
            self._reduce(o[0], o[1], o[2], n, gone)
            if gone:
                del self.orders[ref]
            else:
                o[3] -= n
        elif t == "X":
            ref, shares = struct.unpack_from(">QI", m, 11)
            o = self.orders.get(ref)
            if o is None:
                return
            n = min(shares, o[3])
            gone = n == o[3]
            self._reduce(o[0], o[1], o[2], n, gone)
            if gone:
                del self.orders[ref]
            else:
                o[3] -= n
        elif t == "D":
            ref = struct.unpack_from(">Q", m, 11)[0]
            o = self.orders.pop(ref, None)
            if o is not None:
                self._reduce(o[0], o[1], o[2], o[3], True)
        elif t == "U":
            old, new, shares, price = struct.unpack_from(">QQII", m, 11)
            o = self.orders.pop(old, None)
            if o is None:
                return
            self._reduce(o[0], o[1], o[2], o[3], True)
            if shares == 0 or new in self.orders:
                return
            self.orders[new] = [o[0], o[1], price, shares]
            self._add(o[0], o[1], price, shares)

    def depth_line(self, loc, n=5):
        def side(s, rev):
            d = self.levels.get((loc, s), {})
            return ",".join(f"{p}@{v[0]}/{v[1]}" for p, v in sorted(d.items(), reverse=rev)[:n])

        return f"B:{side('B', True)} A:{side('S', False)}"


def frames(f):
    """Yield BinaryFILE payloads. Zero-length frames are skipped; a partial tail ends the stream."""
    while True:
        h = f.read(2)
        if len(h) < 2:
            return
        n = (h[0] << 8) | h[1]
        if n == 0:
            continue
        m = f.read(n)
        if len(m) < n:
            return
        yield m


def run(path, symbols, every, out):
    opener = gzip.open if path.endswith(".gz") else open
    book = RefBook(symbols)
    idx = 0
    checkpoints = 0
    with opener(path, "rb") as f:
        for m in frames(f):
            idx += 1
            t = chr(m[0])
            if LENGTHS.get(t) == len(m):
                book.handle(m)
            if idx % every == 0:
                checkpoints += 1
                for loc, sym in sorted(book.locate_to_sym.items(), key=lambda kv: kv[1]):
                    out.write(f"{idx} {sym} {book.depth_line(loc)}\n")
    return idx, checkpoints


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--symbols", required=True, help="comma separated")
    ap.add_argument("--every", type=int, required=True, help="checkpoint every N messages")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    with open(a.out, "w") as out:
        n, c = run(a.file, [s for s in a.symbols.split(",") if s], a.every, out)
    print(f"reference: {n} messages, {c} checkpoints", file=sys.stderr)
