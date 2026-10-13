#pragma once
// Deterministic state hash. FNV-1a over explicitly little-endian-serialised integers, so the
// digest is identical across runs, compilers, platforms and book/store implementations.
#include <cstdint>

namespace itch {

class Fnv1a64 {
  public:
    void u8(uint8_t v) {
        h_ ^= v;
        h_ *= 0x100000001b3ull;
    }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) u8(static_cast<uint8_t>(v >> (8 * i)));
    }
    void u64(uint64_t v) {
        for (int i = 0; i < 8; ++i) u8(static_cast<uint8_t>(v >> (8 * i)));
    }
    uint64_t value() const { return h_; }

  private:
    uint64_t h_ = 0xcbf29ce484222325ull;
};

}  // namespace itch
