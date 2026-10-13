// replay: parse a Nasdaq ITCH 5.0 BinaryFILE (.gz or decompressed), rebuild every book, print
// message counts, anomalies and the final-state hash. Optional outputs for selected symbols:
// BBO change log, trade tape, and top-5 depth checkpoints for the differential test.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "itch/book_handler.hpp"
#include "itch/reader.hpp"

using namespace itch;

namespace {

std::string trim_symbol(const char* s) {
    std::string out(s, 8);
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

struct Options {
    std::string path;
    std::set<std::string> symbols;
    std::string bbo_out, tape_out, checkpoint_out;
    uint64_t checkpoint_every = 0;
    std::string store = "flat", levels = "vec";
    bool timing = true;
};

struct FileSink {
    // The sink flags are constexpr on the type, so the benchmark build (NullSink) pays nothing.
    static constexpr bool kWantsBbo = true;
    static constexpr bool kWantsTape = true;
    struct Shared {
        std::set<std::string> wanted;  // empty = everything
        std::vector<bool> selected = std::vector<bool>(65536, false);
        std::vector<std::string> names = std::vector<std::string>(65536);
        std::ofstream bbo, tape, ckpt;
        bool want_bbo = false, want_tape = false;
    };
    std::shared_ptr<Shared> s = std::make_shared<Shared>();

    void on_symbol(Locate l, const char (&stock)[8]) {
        s->names[l] = trim_symbol(stock);
        s->selected[l] = s->wanted.empty() || s->wanted.count(s->names[l]) != 0;
    }
    void on_bbo(uint64_t ts, Locate l, const Bbo& b) {
        if (!s->want_bbo || !s->selected[l]) return;
        s->bbo << ts << ',' << s->names[l] << ',' << b.bid_px << ',' << b.bid_sz << ',' << b.ask_px
               << ',' << b.ask_sz << '\n';
    }
    void on_trade(const TradeRec& t) {
        if (!s->want_tape || !s->selected[t.locate]) return;
        s->tape << t.ts << ',' << s->names[t.locate] << ',' << t.kind << ',' << t.price << ','
                << t.shares << ',' << t.match << ',' << (t.printable ? 'Y' : 'N') << '\n';
    }
    // One line per selected symbol, sorted by symbol: "<idx> <SYM> B:px@sh/n,... A:px@sh/n,..."
    template <class H>
    void on_checkpoint(uint64_t idx, const H& h) {
        if (!s->ckpt.is_open()) return;
        std::vector<std::pair<std::string, Locate>> sel;
        for (size_t l = 1; l < 65536; ++l)
            if (s->selected[l] && !s->names[l].empty())
                sel.emplace_back(s->names[l], static_cast<Locate>(l));
        std::sort(sel.begin(), sel.end());
        for (const auto& [name, l] : sel) {
            s->ckpt << idx << ' ' << name << " B:";
            emit_top5(h.book(l).bids);
            s->ckpt << " A:";
            emit_top5(h.book(l).asks);
            s->ckpt << '\n';
        }
    }
    template <class Side>
    void emit_top5(const Side& side) {
        int n = 0;
        bool first = true;
        side.for_each([&](Price px, const Level& lv) {
            if (n++ >= 5) return;
            if (!first) s->ckpt << ',';
            first = false;
            s->ckpt << px << '@' << lv.shares << '/' << lv.orders;
        });
    }
};

template <class Store, class Bid, class Ask>
int run(const Options& o) {
    FileSink sink;
    sink.s->wanted = o.symbols;
    if (!o.bbo_out.empty()) sink.s->bbo.open(o.bbo_out), sink.s->want_bbo = true;
    if (!o.tape_out.empty()) sink.s->tape.open(o.tape_out), sink.s->want_tape = true;
    if (!o.checkpoint_out.empty()) sink.s->ckpt.open(o.checkpoint_out);
    using Handler = BookBuilder<Store, Bid, Ask, FileSink>;
    auto h = std::make_unique<Handler>(sink, size_t{1} << 20, o.checkpoint_every);

    const auto t0 = std::chrono::steady_clock::now();
    FrameStats fs;
    const bool gz = o.path.size() > 3 && o.path.compare(o.path.size() - 3, 3, ".gz") == 0;
    if (gz) {
        fs = replay_gz(o.path, *h);
    } else {
        MappedFile f(o.path);
        fs = replay_mapped(f, *h);
    }
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::printf("file: %s\n", o.path.c_str());
    std::printf("design: store=%s levels=%s\n", o.store.c_str(), o.levels.c_str());
    std::printf("messages: %llu\n", static_cast<unsigned long long>(fs.messages));
    std::printf("zero_length_frames: %llu\ntrailing_bytes: %zu\n",
                static_cast<unsigned long long>(fs.zero_length_frames), fs.trailing_bytes);
    std::printf("counts:\n");
    for (int c = 0; c < 256; ++c)
        if (h->counts()[static_cast<size_t>(c)])
            std::printf("  %c %llu\n", c,
                        static_cast<unsigned long long>(h->counts()[static_cast<size_t>(c)]));
    const Anomalies& a = h->anomalies();
    std::printf("anomalies:\n");
#define P(f) std::printf("  %s %llu\n", #f, static_cast<unsigned long long>(a.f))
    P(unknown_exec);
    P(unknown_exec_price);
    P(unknown_cancel);
    P(unknown_delete);
    P(unknown_replace);
    P(duplicate_ref);
    P(zero_share_add);
    P(exec_exceeds_remaining);
    P(cancel_exceeds_remaining);
    P(level_inconsistent);
    P(crossed_after_msg);
    P(malformed);
    P(unknown_type);
#undef P
    std::printf("live_orders: %zu\n", h->store().size());
    std::printf("printable_shares: %llu\nnonprintable_shares: %llu\n",
                static_cast<unsigned long long>(h->printable_shares()),
                static_cast<unsigned long long>(h->nonprintable_shares()));
    std::printf("state_hash: %016llx\n", static_cast<unsigned long long>(h->state_hash()));
    if (o.timing)
        std::printf(
            "elapsed_s: %.3f\nmsgs_per_s: %.0f (single run, includes %s; use e2e_bench for "
            "results)\n",
            secs, static_cast<double>(fs.messages) / secs, gz ? "gunzip" : "mmap");
    return 0;
}

template <class Store>
int pick_levels(const Options& o) {
    if (o.levels == "map") return run<Store, SideMap<true>, SideMap<false>>(o);
    if (o.levels == "vec") return run<Store, SideVec<true>, SideVec<false>>(o);
    std::fprintf(stderr, "--levels must be map|vec\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--symbols") {
            std::string v = val(), tok;
            for (char c : v + ",") {
                if (c == ',') {
                    if (!tok.empty()) o.symbols.insert(tok);
                    tok.clear();
                } else {
                    tok += c;
                }
            }
        } else if (a == "--bbo-out")
            o.bbo_out = val();
        else if (a == "--tape-out")
            o.tape_out = val();
        else if (a == "--checkpoint-out")
            o.checkpoint_out = val();
        else if (a == "--checkpoint-every")
            o.checkpoint_every = std::stoull(val());
        else if (a == "--store")
            o.store = val();
        else if (a == "--levels")
            o.levels = val();
        else if (a == "--no-timing")
            o.timing = false;
        else if (!a.empty() && a[0] != '-')
            o.path = a;
        else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }
    if (o.path.empty()) {
        std::fprintf(stderr,
                     "usage: replay <file[.gz]> [--symbols A,B] [--bbo-out f] [--tape-out f]\n"
                     "              [--checkpoint-every N --checkpoint-out f --symbols ...]\n"
                     "              [--store std|flat] [--levels map|vec] [--no-timing]\n");
        return 2;
    }
    if (o.checkpoint_every != 0 && (o.checkpoint_out.empty() || o.symbols.empty())) {
        std::fprintf(stderr, "--checkpoint-every needs --checkpoint-out and --symbols\n");
        return 2;
    }
    try {
        if (o.store == "std") return pick_levels<StdOrderStore>(o);
        if (o.store == "flat") return pick_levels<FlatOrderStore>(o);
        std::fprintf(stderr, "--store must be std|flat\n");
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
