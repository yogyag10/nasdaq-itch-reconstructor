// libFuzzer target: feed arbitrary bytes through the framer, parser and book builder. It must
// never crash, trip a sanitizer, or read out of bounds, whatever the input.
//
// Built two ways (see CMakeLists.txt): with -fsanitize=fuzzer on compilers that ship libFuzzer,
// or with ITCH_STANDALONE_FUZZ_DRIVER, a main() that replays files/directories given on the
// command line through the same entry point (used on Apple clang and for corpus regression).
#include <cstddef>
#include <cstdint>
#include <memory>

#include "itch/book_handler.hpp"
#include "itch/parser.hpp"

namespace {
using Handler = itch::BookBuilder<itch::FlatOrderStore, itch::SideVec<true>, itch::SideVec<false>>;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Fresh state per input keeps runs independent and reproducible. The small reserve keeps the
    // per-input setup cost low; the 65536-book table is the dominant constant.
    static std::unique_ptr<Handler> h;
    static uint64_t uses = 0;
    if (!h || ++uses % 256 == 0) h = std::make_unique<Handler>(itch::NullSink{}, 64);
    itch::FrameStats st;
    itch::consume_frames({reinterpret_cast<const std::byte*>(data), size}, *h, st);
#ifndef NDEBUG
    if (!h->verify().empty()) __builtin_trap();  // invariants must hold even on garbage input
#endif
    return 0;
}

#ifdef ITCH_STANDALONE_FUZZ_DRIVER
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc, char** argv) {
    size_t n = 0;
    auto run_file = [&](const std::filesystem::path& p) {
        std::ifstream f(p, std::ios::binary);
        std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
        LLVMFuzzerTestOneInput(buf.data(), buf.size());
        ++n;
    };
    for (int i = 1; i < argc; ++i) {
        std::filesystem::path p(argv[i]);
        if (std::filesystem::is_directory(p)) {
            for (auto& e : std::filesystem::recursive_directory_iterator(p))
                if (e.is_regular_file()) run_file(e.path());
        } else {
            run_file(p);
        }
    }
    std::printf("replayed %zu inputs without a crash\n", n);
}
#endif
