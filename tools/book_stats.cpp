// book_stats: one replay pass that answers two questions about a day file.
//   1. How many distinct locates are in the stock directory (R), ever receive an add, and ever
//      have a non-empty book?  (the "symbols" count a README or resume may claim)
//   2. When do crossed or locked books happen? Each message that leaves best bid >= best ask is
//      one event (the same definition as the replay tool's crossed_after_msg counter), classified
//      by session phase, per-symbol trading state (H), and distance from the last cross (Q).
// It wraps BookBuilder and never changes book logic. Analysis only: not on the benchmark path.
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "itch/book_handler.hpp"
#include "itch/reader.hpp"

using namespace itch;

namespace {

using Base = BookBuilder<FlatOrderStore, SideVec<true>, SideVec<false>, NullSink>;

std::string hms(uint64_t ns) {
    const uint64_t s = ns / 1000000000ull;
    char b[32];
    std::snprintf(b, sizeof b, "%02llu:%02llu:%02llu", (unsigned long long)(s / 3600),
                  (unsigned long long)(s / 60 % 60), (unsigned long long)(s % 60));
    return b;
}

struct Episode {
    Locate loc;
    uint64_t start_ts, end_ts, msgs;
    bool locked_only;  // never strictly crossed during the episode
    bool open_at_eod;
};

struct Event {
    uint64_t ts;
    Locate loc;
    bool locked;  // bid == ask (else bid > ask)
    std::string phase;
    char state;     // last H state for the symbol, '?' if none yet
    char msg_type;  // message that left the book crossed
    uint64_t since_cross_ns, since_halt_ns;
};

class Analyzer : public Base {
  public:
    Analyzer() : Base(NullSink{}, size_t{1} << 20) {}

    void begin(uint8_t t) {
        Base::begin(t);
        cur_ = 0;
        type_ = static_cast<char>(t);
    }
    void on_system_event(const SystemEvent& e) {
        phase_ = e.code;
        std::printf("system_event %c at %s\n", e.code, hms(e.h.ts).c_str());
    }
    void on_stock_directory(const StockDirectory& d) {
        Base::on_stock_directory(d);
        in_r_[d.h.locate] = true;
        name_[d.h.locate].assign(d.stock, 8);
        while (!name_[d.h.locate].empty() && name_[d.h.locate].back() == ' ')
            name_[d.h.locate].pop_back();
    }
    void on_trading_action(const TradingAction& a) {
        h_log_.push_back({a.h.ts, a.h.locate, a.state});
        state_[a.h.locate] = a.state;
        last_halt_[a.h.locate] = a.h.ts;
        ++h_count_[a.state];
    }
    void on_cross(const CrossTrade& c) {
        q_log_.push_back({c.h.ts, c.h.locate, c.cross_type});
        last_cross_[c.h.locate] = c.h.ts;
        ++q_count_[c.cross_type];
    }
    void on_add(const AddOrder& a) {
        Base::on_add(a);
        touch(a.h.locate, a.h.ts);
        had_add_[a.h.locate] = true;
        if (!book(a.h.locate).empty()) ever_nonempty_[a.h.locate] = true;
    }
    void on_executed(const OrderExecuted& e) {
        Base::on_executed(e);
        touch(e.h.locate, e.h.ts);
    }
    void on_executed_price(const OrderExecutedPrice& e) {
        Base::on_executed_price(e);
        touch(e.h.locate, e.h.ts);
    }
    void on_cancel(const OrderCancel& c) {
        Base::on_cancel(c);
        touch(c.h.locate, c.h.ts);
    }
    void on_delete(const OrderDelete& d) {
        Base::on_delete(d);
        touch(d.h.locate, d.h.ts);
    }
    void on_replace(const OrderReplace& u) {
        Base::on_replace(u);
        touch(u.h.locate, u.h.ts);
        if (!book(u.h.locate).empty()) ever_nonempty_[u.h.locate] = true;
    }
    void end() {
        const uint64_t before = anomalies().crossed_after_msg;
        Base::end();
        if (cur_ == 0) return;
        const Bbo b = book(cur_).bbo();
        const bool crossed = b.bid_sz != 0 && b.ask_sz != 0 && b.bid_px >= b.ask_px;
        Open& ep = open_[cur_];
        if (crossed) {
            if (anomalies().crossed_after_msg != before + 1) ++mismatch_with_counter_;
            const bool locked = b.bid_px == b.ask_px;
            events_.push_back(Event{cur_ts_, cur_, locked, phase_name(), state_[cur_], type_,
                                    last_cross_[cur_] ? cur_ts_ - last_cross_[cur_] : ~0ull,
                                    last_halt_[cur_] ? cur_ts_ - last_halt_[cur_] : ~0ull});
            if (!ep.active) ep = Open{true, cur_ts_, 0, true};
            ++ep.msgs;
            if (!locked) ep.locked_only = false;
        } else {
            if (anomalies().crossed_after_msg != before) ++mismatch_with_counter_;
            if (ep.active) {
                episodes_.push_back(
                    Episode{cur_, ep.start_ts, cur_ts_, ep.msgs, ep.locked_only, false});
                ep.active = false;
            }
        }
    }

    void finish() {
        for (size_t l = 0; l < open_.size(); ++l)
            if (open_[l].active)
                episodes_.push_back(Episode{static_cast<Locate>(l), open_[l].start_ts, 0,
                                            open_[l].msgs, open_[l].locked_only, true});
    }

    void report() const {
        size_t r = 0, a = 0, ne = 0;
        for (size_t l = 0; l < in_r_.size(); ++l) {
            r += in_r_[l];
            a += had_add_[l];
            ne += ever_nonempty_[l];
        }
        std::printf("\n== symbol counts ==\n");
        std::printf("(a) distinct locates in R messages:           %zu\n", r);
        std::printf("(b) distinct locates that received an add:    %zu\n", a);
        std::printf("(c) distinct locates whose book was non-empty: %zu\n", ne);
        std::printf("locates with adds but no R message: %zu\n", [&] {
            size_t n = 0;
            for (size_t l = 0; l < in_r_.size(); ++l) n += had_add_[l] && !in_r_[l];
            return n;
        }());
        std::printf("\n== trading actions (H) by state ==\n");
        for (auto [k, v] : h_count_) std::printf("  %c %llu\n", k, (unsigned long long)v);
        std::printf("== cross trades (Q) by cross type ==\n");
        for (auto [k, v] : q_count_) std::printf("  %c %llu\n", k, (unsigned long long)v);

        std::printf("\n== crossed/locked events (messages leaving best bid >= best ask) ==\n");
        std::printf("events: %zu (replay crossed_after_msg counter disagreements: %llu)\n",
                    events_.size(), (unsigned long long)mismatch_with_counter_);
        size_t locked = 0;
        for (const Event& e : events_) locked += e.locked;
        std::printf("locked (bid == ask): %zu   crossed (bid > ask): %zu\n", locked,
                    events_.size() - locked);
        std::map<std::string, size_t> by_phase;
        std::map<char, size_t> by_state, by_type;
        size_t within1s_cross = 0, within1s_halt = 0, never_cross = 0;
        std::map<Locate, size_t> by_sym;
        for (const Event& e : events_) {
            ++by_phase[e.phase];
            ++by_state[e.state];
            ++by_type[e.msg_type];
            ++by_sym[e.loc];
            if (e.since_cross_ns <= 1000000000ull) ++within1s_cross;
            if (e.since_halt_ns <= 1000000000ull) ++within1s_halt;
            if (e.since_cross_ns == ~0ull) ++never_cross;
        }
        std::printf("by session phase:\n");
        for (auto& [k, v] : by_phase) std::printf("  %-26s %zu\n", k.c_str(), v);
        std::printf("by symbol's last trading-action state (? = no H seen yet):\n");
        for (auto& [k, v] : by_state) std::printf("  %c %zu\n", k, v);
        std::map<std::pair<char, std::string>, size_t> state_phase;
        std::map<Locate, size_t> t_by_sym;
        for (const Event& e : events_)
            if (e.state == 'T') {
                ++state_phase[{e.state, e.phase}];
                ++t_by_sym[e.loc];
            }
        size_t t_near_q = 0, t_total = 0;
        for (const Event& e : events_)
            if (e.state == 'T') {
                ++t_total;
                t_near_q += e.since_cross_ns <= 1000000000ull;
            }
        std::printf("events with state T within 1s after a Q on the same symbol: %zu of %zu\n",
                    t_near_q, t_total);
        std::printf("events with state T, by phase:\n");
        for (auto& [k, v] : state_phase) std::printf("  %-26s %zu\n", k.second.c_str(), v);
        std::printf("events with state T, by symbol:\n");
        for (auto& [l, v] : t_by_sym) std::printf("  %-8s %zu\n", name_[l].c_str(), v);
        std::printf("by message type that left the book crossed:\n");
        for (auto& [k, v] : by_type) std::printf("  %c %zu\n", k, v);
        std::printf("within 1s after a cross trade (Q) on the same symbol: %zu\n", within1s_cross);
        std::printf("within 1s after a trading action (H) on the same symbol: %zu\n",
                    within1s_halt);
        std::printf("on symbols that never had a Q before the event: %zu\n", never_cross);
        std::vector<std::pair<size_t, Locate>> top;
        for (auto& [l, n] : by_sym) top.emplace_back(n, l);
        std::sort(top.rbegin(), top.rend());
        std::printf("distinct symbols with at least one event: %zu\ntop symbols by events:\n",
                    by_sym.size());
        for (size_t i = 0; i < top.size() && i < 10; ++i)
            std::printf("  %-8s %zu\n", name_[top[i].second].c_str(), top[i].first);

        std::printf("\n== H and Q timeline for the symbols with events ==\n");
        for (auto& [n, l] : top) {
            std::printf("%s:", name_[l].c_str());
            for (const Log& h : h_log_)
                if (h.loc == l) std::printf(" H=%c@%s", h.code, hms(h.ts).c_str());
            for (const Log& q : q_log_)
                if (q.loc == l) std::printf(" Q=%c@%s", q.code, hms(q.ts).c_str());
            std::printf("\n");
        }

        std::printf("\n== episodes (consecutive crossed messages on one symbol) ==\n");
        std::printf("episodes: %zu (still crossed at end of day: %zu, locked-only: %zu)\n",
                    episodes_.size(),
                    (size_t)std::count_if(episodes_.begin(), episodes_.end(),
                                          [](const Episode& e) { return e.open_at_eod; }),
                    (size_t)std::count_if(episodes_.begin(), episodes_.end(),
                                          [](const Episode& e) { return e.locked_only; }));
        std::vector<uint64_t> msgs, dur;
        for (const Episode& e : episodes_) {
            msgs.push_back(e.msgs);
            if (!e.open_at_eod) dur.push_back(e.end_ts - e.start_ts);
        }
        auto pct = [](std::vector<uint64_t> v, double p) {
            if (v.empty()) return uint64_t{0};
            std::sort(v.begin(), v.end());
            return v[static_cast<size_t>(p * static_cast<double>(v.size() - 1))];
        };
        std::printf("crossed-message count per episode: p50 %llu p90 %llu max %llu\n",
                    (unsigned long long)pct(msgs, .5), (unsigned long long)pct(msgs, .9),
                    (unsigned long long)pct(msgs, 1));
        std::printf("duration (ns) of closed episodes:  p50 %llu p90 %llu max %llu\n",
                    (unsigned long long)pct(dur, .5), (unsigned long long)pct(dur, .9),
                    (unsigned long long)pct(dur, 1));
        std::vector<Episode> longest = episodes_;
        std::sort(longest.begin(), longest.end(), [](const Episode& x, const Episode& y) {
            return (x.open_at_eod ? ~0ull : x.end_ts - x.start_ts) >
                   (y.open_at_eod ? ~0ull : y.end_ts - y.start_ts);
        });
        std::printf("longest episodes:\n");
        for (size_t i = 0; i < longest.size() && i < 8; ++i) {
            const Episode& e = longest[i];
            std::printf("  %-8s start %s msgs %llu %s\n", name_[e.loc].c_str(),
                        hms(e.start_ts).c_str(), (unsigned long long)e.msgs,
                        e.open_at_eod ? "OPEN AT END OF DAY"
                                      : ("lasted " +
                                         std::to_string((e.end_ts - e.start_ts) / 1000000) + " ms")
                                            .c_str());
        }
    }

  private:
    struct Open {
        bool active = false;
        uint64_t start_ts = 0, msgs = 0;
        bool locked_only = true;
    };
    std::string phase_name() const {
        switch (phase_) {
            case 'O':
                return "start of messages";
            case 'S':
                return "system hours (pre-market)";
            case 'Q':
                return "regular market hours";
            case 'M':
                return "post-market (after close)";
            case 'E':
                return "after end of system hours";
            default:
                return "before any system event";
        }
    }
    void touch(Locate l, uint64_t ts) {
        cur_ = l;
        cur_ts_ = ts;
    }

    Locate cur_ = 0;
    uint64_t cur_ts_ = 0;
    char type_ = 0, phase_ = 0;
    std::vector<bool> in_r_ = std::vector<bool>(65536), had_add_ = std::vector<bool>(65536),
                      ever_nonempty_ = std::vector<bool>(65536);
    std::vector<std::string> name_ = std::vector<std::string>(65536);
    std::vector<char> state_ = std::vector<char>(65536, '?');
    std::vector<uint64_t> last_cross_ = std::vector<uint64_t>(65536, 0),
                          last_halt_ = std::vector<uint64_t>(65536, 0);
    std::vector<Open> open_ = std::vector<Open>(65536);
    std::map<char, uint64_t> h_count_, q_count_;
    struct Log {
        uint64_t ts;
        Locate loc;
        char code;
    };
    std::vector<Log> h_log_, q_log_;
    std::vector<Event> events_;
    std::vector<Episode> episodes_;
    uint64_t mismatch_with_counter_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: book_stats <decompressed ITCH file>\n");
        return 2;
    }
    auto a = std::make_unique<Analyzer>();
    MappedFile f(argv[1]);
    FrameStats st = replay_mapped(f, *a);
    a->finish();
    std::printf("messages: %llu  state_hash: %016llx\n", (unsigned long long)st.messages,
                (unsigned long long)a->state_hash());
    a->report();
    return 0;
}
