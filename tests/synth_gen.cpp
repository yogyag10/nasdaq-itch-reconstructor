// synth_gen <out.bin> [--seed N] [--symbols N] [--messages N]
// Writes a deterministic synthetic BinaryFILE built with the test encoder.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "synth.hpp"

int main(int argc, char** argv) {
    itch::test::SynthConfig cfg;
    std::string out;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--seed")
            cfg.seed = std::stoull(val());
        else if (a == "--symbols")
            cfg.symbols = static_cast<unsigned>(std::stoul(val()));
        else if (a == "--messages")
            cfg.messages = std::stoull(val());
        else
            out = a;
    }
    if (out.empty()) {
        std::fprintf(stderr,
                     "usage: synth_gen <out.bin> [--seed N] [--symbols N] [--messages N]\n");
        return 2;
    }
    const auto s = itch::test::make_day(cfg);
    FILE* f = std::fopen(out.c_str(), "wb");
    if (!f) return 1;
    std::fwrite(s.bytes().data(), 1, s.bytes().size(), f);
    std::fclose(f);
    std::printf("wrote %zu messages, %zu bytes to %s\n", s.count(), s.bytes().size(), out.c_str());
}
