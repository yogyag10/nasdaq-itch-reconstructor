#pragma once
// Seeded synthetic trading day built with the test encoder. Used for unit tests, the
// differential test, the CI fixture and the benchmark smoke job. Real Nasdaq data is the
// source of record for results; this exists so everything else is reproducible offline.
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "encoder.hpp"

namespace itch::test {

struct SynthConfig {
    uint64_t seed = 1;
    unsigned symbols = 20;
    size_t messages = 100000;         // approximate; R/S headers come on top
    double bad_ref_fraction = 0.002;  // messages deliberately referencing unknown orders
};

inline Stream make_day(const SynthConfig& cfg) {
    std::mt19937_64 rng(cfg.seed);
    auto uni = [&](uint64_t n) { return static_cast<uint64_t>(rng() % n); };
    Stream s;
    uint64_t ts = 4ull * 3600 * 1000000000ull;  // 04:00
    s.add(system_event(ts, 'O'));
    std::vector<std::string> names;
    for (unsigned i = 0; i < cfg.symbols; ++i) {
        std::string n = i == 0 ? "AAPL" : i == 1 ? "MSFT" : "ILQ" + std::to_string(100 + i);
        names.push_back(n);
        s.add(stock_directory(static_cast<uint16_t>(i + 1), ts += 1000, n));
    }
    struct Live {
        uint64_t ref;
        uint32_t shares;
        char side;
        uint32_t price;
    };
    std::vector<std::vector<Live>> live(cfg.symbols);
    std::vector<uint32_t> mid(cfg.symbols);
    for (unsigned i = 0; i < cfg.symbols; ++i)
        mid[i] = 100000 + static_cast<uint32_t>(uni(2000000));
    uint64_t next_ref = 100000, match = 1;
    s.add(system_event(ts += 1000, 'S'));
    s.add(system_event(ts += 1000, 'Q'));

    for (size_t n = 0; n < cfg.messages; ++n) {
        ts += 1 + uni(5000);
        // Skew activity: first two symbols are "liquid" and get most of the flow.
        unsigned si = uni(100) < 50 ? static_cast<unsigned>(uni(2)) % cfg.symbols
                                    : static_cast<unsigned>(uni(cfg.symbols));
        const uint16_t loc = static_cast<uint16_t>(si + 1);
        auto& lv = live[si];
        const uint64_t r = uni(1000);
        auto pick = [&]() { return uni(lv.size()); };
        auto drop = [&](size_t i) { lv[i] = lv.back(), lv.pop_back(); };
        if (static_cast<double>(uni(1000000)) < cfg.bad_ref_fraction * 1e6) {
            const uint64_t bad = 1 + uni(50000);  // below the first real ref: never live
            switch (uni(5)) {
                case 0:
                    s.add(executed(loc, ts, bad, 100, match++));
                    break;
                case 1:
                    s.add(cancel(loc, ts, bad, 10));
                    break;
                case 2:
                    s.add(del(loc, ts, bad));
                    break;
                case 3:
                    s.add(replace(loc, ts, bad, next_ref++, 100, mid[si]));
                    break;
                default:
                    s.add(executed_price(loc, ts, bad, 100, match++, 'Y', mid[si]));
                    break;
            }
            continue;
        }
        if (lv.empty() || r < 450) {  // add
            const bool buy = uni(2) == 0;
            const uint32_t off = 100 * static_cast<uint32_t>(1 + uni(30));
            const uint32_t px = buy ? mid[si] - off : mid[si] + off;
            const uint32_t sh = 100 * static_cast<uint32_t>(1 + uni(20));
            const uint64_t ref = next_ref++;
            if (uni(10) == 0)
                s.add(add_mpid(loc, ts, ref, buy ? 'B' : 'S', sh, names[si], px, "NSDQ"));
            else
                s.add(add(loc, ts, ref, buy ? 'B' : 'S', sh, names[si], px));
            lv.push_back({ref, sh, buy ? 'B' : 'S', px});
        } else if (r < 600) {  // delete
            size_t i = pick();
            s.add(del(loc, ts, lv[i].ref));
            drop(i);
        } else if (r < 750) {  // execute (full or partial)
            size_t i = pick();
            const bool full = uni(2) == 0 || lv[i].shares <= 100;
            uint32_t qq = lv[i].shares;
            if (!full)
                qq = 100 * static_cast<uint32_t>(1 + uni(lv[i].shares / 100 - 1));  // < shares
            s.add(executed(loc, ts, lv[i].ref, qq, match++));
            if (qq == lv[i].shares)
                drop(i);
            else
                lv[i].shares -= qq;
        } else if (r < 800) {  // execute with price (sometimes non-printable)
            size_t i = pick();
            const uint32_t qq = lv[i].shares >= 200 ? 100 : lv[i].shares;
            s.add(executed_price(loc, ts, lv[i].ref, qq, match++, uni(4) == 0 ? 'N' : 'Y',
                                 lv[i].price + 100));
            if (qq == lv[i].shares)
                drop(i);
            else
                lv[i].shares -= qq;
        } else if (r < 860) {  // partial cancel
            size_t i = pick();
            if (lv[i].shares <= 100) {
                s.add(del(loc, ts, lv[i].ref));
                drop(i);
            } else {
                s.add(cancel(loc, ts, lv[i].ref, 100));
                lv[i].shares -= 100;
            }
        } else if (r < 960) {  // replace: new ref, new price, new size, same side
            size_t i = pick();
            const uint32_t off = 100 * static_cast<uint32_t>(1 + uni(30));
            const uint32_t px = lv[i].side == 'B' ? mid[si] - off : mid[si] + off;
            const uint32_t sh = 100 * static_cast<uint32_t>(1 + uni(20));
            const uint64_t nr = next_ref++;
            s.add(replace(loc, ts, lv[i].ref, nr, sh, px));
            lv[i] = {nr, sh, lv[i].side, px};
        } else if (r < 985) {  // non-cross trade against hidden liquidity
            s.add(trade(loc, ts, 'B', 100 * static_cast<uint32_t>(1 + uni(10)), names[si], mid[si],
                        match++));
        } else if (r < 995) {  // mid drifts by a tick
            if (uni(2))
                mid[si] += 100;
            else if (mid[si] > 10000)
                mid[si] -= 100;
        } else if (r < 998) {
            s.add(trading_action(loc, ts, names[si], 'T'));
        } else {
            s.add(match > 1 ? broken(loc, ts, 1 + uni(match - 1)) : system_event(ts, 'Q'));
        }
    }
    s.add(cross(1, ts += 1000, 12345, names[0], mid[0], match++, 'C'));
    s.add(system_event(ts += 1000, 'M'));
    s.add(system_event(ts += 1000, 'E'));
    s.add(system_event(ts += 1000, 'C'));
    return s;
}

}  // namespace itch::test
