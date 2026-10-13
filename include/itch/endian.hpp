#pragma once
// Explicit big-endian loads. ITCH integers are network byte order (spec "Data Types").
// Manual shifts instead of reinterpret_cast: no alignment or aliasing UB, and the compiler
// folds these into a single load + byte-swap. Callers must have bounds-checked already.
#include <cstddef>
#include <cstdint>
#include <span>

namespace itch {

using Bytes = std::span<const std::byte>;

inline uint64_t b(const std::byte* p, size_t i) {
    return static_cast<uint64_t>(p[i]);
}

inline uint8_t be8(const std::byte* p) {
    return static_cast<uint8_t>(p[0]);
}
inline uint16_t be16(const std::byte* p) {
    return static_cast<uint16_t>((b(p, 0) << 8) | b(p, 1));
}
inline uint32_t be32(const std::byte* p) {
    return static_cast<uint32_t>((b(p, 0) << 24) | (b(p, 1) << 16) | (b(p, 2) << 8) | b(p, 3));
}
// 6-byte timestamp: nanoseconds since midnight.
inline uint64_t be48(const std::byte* p) {
    return (b(p, 0) << 40) | (b(p, 1) << 32) | (b(p, 2) << 24) | (b(p, 3) << 16) | (b(p, 4) << 8) |
           b(p, 5);
}
inline uint64_t be64(const std::byte* p) {
    return (b(p, 0) << 56) | (b(p, 1) << 48) | (b(p, 2) << 40) | (b(p, 3) << 32) | (b(p, 4) << 24) |
           (b(p, 5) << 16) | (b(p, 6) << 8) | b(p, 7);
}

}  // namespace itch
