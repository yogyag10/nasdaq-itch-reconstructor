// e2e_bench: repeated end-to-end runs of one design over a memory-resident file, one CSV row per
// run, for the bootstrap analysis in analysis/. It never prints a "headline" number itself.
//
//   throughput mode: no timers inside the loop; wall time of the whole replay / messages.
//   latency mode   : times 1 message in every N (sample) or every batch of N messages (batch)
//                    with the cycle/tick counter, into an HDR-style histogram. Sampling keeps the
//                    timer cost off ~(N-1)/N of messages; the timer's own back-to-back cost is
//                    measured and reported (timer_floor_p50_ns) so it can be judged, not hidden.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "itch/book_handler.hpp"
#include "itch/hdr_histogram.hpp"
#include "itch/reader.hpp"
#include "itch/timer.hpp"
#include "synth.hpp"

using namespace itch;

namespace {

struct Opt {
    std::string file, label, csv, hist_out;
    uint64_t synth = 0;
    std::string store = "flat", levels = "vec", mode = "throughput", lat_mode = "sample";
    int reps = 30, warmup = 3;
    uint64_t every = 64;  // sample 1-in-N messages, or batch size
    bool header = true;
};

struct View {
    Bytes data;
};

template <class Handler>
double run_throughput(View v, uint64_t& msgs, uint64_t& hash) {
    auto h = std::make_unique<Handler>(NullSink{}, size_t{1} << 20);
    FrameStats st;
    const auto t0 = std::chrono::steady_clock::now();
    consume_frames(v.data, *h, st);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    msgs = st.messages;
    hash = h->state_hash();
    return s;
}

template <class Handler>
double run_latency(View v, const Opt& o, double ns_tick, HdrHistogram& hist, uint64_t& msgs,
                   uint64_t& hash) {
    auto h = std::make_unique<Handler>(NullSink{}, size_t{1} << 20);
    FrameCursor cur(v.data);
    Bytes m;
    uint64_t i = 0;
    const uint64_t mask = o.every - 1;  // `every` must be a power of two
    const auto t0 = std::chrono::steady_clock::now();
    if (o.lat_mode == "sample") {
        while (cur.next(m)) {
            if ((i++ & mask) == 0) {
                const uint64_t a = ticks();
                parse_message(m, *h);
                const uint64_t b = ticks();
                hist.record(static_cast<uint64_t>(static_cast<double>(b - a) * ns_tick));
            } else {
                parse_message(m, *h);
            }
        }
    } else {  // batch: record mean ns/message of each batch of `every` messages (x1000 => ps)
        uint64_t a = ticks();
        while (cur.next(m)) {
            parse_message(m, *h);
            if ((++i & mask) == 0) {
                const uint64_t b = ticks();
                hist.record(static_cast<uint64_t>(static_cast<double>(b - a) * ns_tick * 1000.0 /
                                                  static_cast<double>(o.every)));
                a = ticks();
            }
        }
    }
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    msgs = i;
    hash = h->state_hash();
    return s;
}

uint64_t timer_floor_p50(double ns_tick) {
    HdrHistogram f;
    for (int i = 0; i < 1000000; ++i) {
        const uint64_t a = ticks();
        asm volatile("" ::: "memory");
        const uint64_t b = ticks();
        f.record(static_cast<uint64_t>(static_cast<double>(b - a) * ns_tick));
    }
    return f.percentile(0.5);
}

template <class Store, class Bid, class Ask>
int go(const Opt& o, View v) {
    using Handler = BookBuilder<Store, Bid, Ask, NullSink>;
    const double ns_tick = ns_per_tick();
    const uint64_t floor_ns = o.mode == "latency" ? timer_floor_p50(ns_tick) : 0;
    const double gran_ns = o.mode == "latency" ? timer_granularity_ns(ns_tick) : 0.0;
    HdrHistogram pooled;
    std::FILE* out = o.csv.empty() ? stdout : std::fopen(o.csv.c_str(), "a");
    if (!out) return 1;
    if (o.header)
        std::fprintf(out,
                     "label,store,levels,mode,rep,messages,seconds,msgs_per_sec,p50_ns,p99_ns,"
                     "p999_ns,timer_floor_p50_ns,timer_granularity_ns,lat_mode,every,hash\n");
    uint64_t ref_hash = 0;
    for (int rep = -o.warmup; rep < o.reps; ++rep) {
        uint64_t msgs = 0, hash = 0;
        HdrHistogram hist;
        const double s = o.mode == "throughput"
                             ? run_throughput<Handler>(v, msgs, hash)
                             : run_latency<Handler>(v, o, ns_tick, hist, msgs, hash);
        if (rep == -o.warmup) ref_hash = hash;
        if (hash != ref_hash) {
            std::fprintf(stderr, "NON-DETERMINISTIC: state hash changed between runs\n");
            return 3;
        }
        if (rep < 0) continue;  // warm-up: not recorded
        if (o.mode == "latency") pooled.merge(hist);
        const double div = (o.mode == "latency" && o.lat_mode == "batch") ? 1000.0 : 1.0;
        std::fprintf(
            out, "%s,%s,%s,%s,%d,%llu,%.6f,%.0f,%.2f,%.2f,%.2f,%llu,%.2f,%s,%llu,%016llx\n",
            o.label.c_str(), o.store.c_str(), o.levels.c_str(), o.mode.c_str(), rep,
            static_cast<unsigned long long>(msgs), s, static_cast<double>(msgs) / s,
            static_cast<double>(hist.percentile(0.50)) / div,
            static_cast<double>(hist.percentile(0.99)) / div,
            static_cast<double>(hist.percentile(0.999)) / div,
            static_cast<unsigned long long>(floor_ns), gran_ns, o.lat_mode.c_str(),
            static_cast<unsigned long long>(o.every), static_cast<unsigned long long>(hash));
        std::fflush(out);
    }
    if (out != stdout) std::fclose(out);
    if (!o.hist_out.empty() && o.mode == "latency") {
        std::ofstream hf(o.hist_out);
        hf << "upper_ns,count\n";
        const double div = o.lat_mode == "batch" ? 1000.0 : 1.0;
        pooled.for_each_bucket([&](uint64_t up, uint64_t c) {
            hf << static_cast<double>(up) / div << ',' << c << '\n';
        });
    }
    return 0;
}

template <class Store>
int pick(const Opt& o, View v) {
    if (o.levels == "map") return go<Store, SideMap<true>, SideMap<false>>(o, v);
    if (o.levels == "vec") return go<Store, SideVec<true>, SideVec<false>>(o, v);
    std::fprintf(stderr, "--levels must be map|vec\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    Opt o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--file")
            o.file = val();
        else if (a == "--synth")
            o.synth = std::stoull(val());
        else if (a == "--store")
            o.store = val();
        else if (a == "--levels")
            o.levels = val();
        else if (a == "--mode")
            o.mode = val();
        else if (a == "--lat-mode")
            o.lat_mode = val();
        else if (a == "--every")
            o.every = std::stoull(val());
        else if (a == "--reps")
            o.reps = std::stoi(val());
        else if (a == "--warmup")
            o.warmup = std::stoi(val());
        else if (a == "--label")
            o.label = val();
        else if (a == "--csv")
            o.csv = val();
        else if (a == "--hist-out")
            o.hist_out = val();
        else if (a == "--no-header")
            o.header = false;
        else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }
    if ((o.file.empty() && o.synth == 0) || (o.every & (o.every - 1)) != 0 || o.every == 0 ||
        (o.mode != "throughput" && o.mode != "latency") ||
        (o.lat_mode != "sample" && o.lat_mode != "batch")) {
        std::fprintf(
            stderr,
            "usage: e2e_bench (--file F | --synth N) [--store std|flat] [--levels map|vec]\n"
            "       [--mode throughput|latency] [--lat-mode sample|batch] [--every 2^k]\n"
            "       [--reps 30] [--warmup 3] [--label L] [--csv out.csv] [--hist-out h.csv]\n");
        return 2;
    }
    if (o.label.empty()) o.label = o.store + "-" + o.levels;
    try {
        std::unique_ptr<MappedFile> mf;
        itch::test::Stream synth;
        View v;
        if (o.synth) {
            itch::test::SynthConfig cfg;
            cfg.messages = o.synth;
            cfg.symbols = 200;
            synth = itch::test::make_day(cfg);
            v.data = {synth.bytes().data(), synth.bytes().size()};
        } else {
            mf = std::make_unique<MappedFile>(o.file);
            if (mf->prefault() == 1) std::fputc(0, stderr);  // keep the touch loop alive
            v.data = mf->bytes();
        }
        return o.store == "std" ? pick<StdOrderStore>(o, v) : pick<FlatOrderStore>(o, v);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
