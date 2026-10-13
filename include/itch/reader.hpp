#pragma once
// Input: mmap of a decompressed BinaryFILE (the benchmarked path), or streaming gunzip via
// zlib (convenience; not used for timing since inflate would dominate the numbers).
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "itch/parser.hpp"

namespace itch {

class MappedFile {
  public:
    explicit MappedFile(const std::string& path) {
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) throw std::runtime_error("cannot open " + path);
        struct stat st{};
        if (::fstat(fd, &st) != 0) {
            ::close(fd);
            throw std::runtime_error("fstat failed: " + path);
        }
        size_ = static_cast<size_t>(st.st_size);
        if (size_ > 0) {
            void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
            if (p == MAP_FAILED) {
                ::close(fd);
                throw std::runtime_error("mmap failed: " + path);
            }
            data_ = static_cast<const std::byte*>(p);
            ::madvise(const_cast<std::byte*>(data_), size_, MADV_SEQUENTIAL);
        }
        ::close(fd);
    }
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    ~MappedFile() {
        if (data_) ::munmap(const_cast<std::byte*>(data_), size_);
    }
    Bytes bytes() const { return {data_, size_}; }
    // Touch every page so a timed run measures parsing, not page faults / disk reads.
    uint64_t prefault() const {
        uint64_t sum = 0;
        for (size_t i = 0; i < size_; i += 4096) sum += static_cast<uint8_t>(data_[i]);
        return sum;
    }

  private:
    const std::byte* data_ = nullptr;
    size_t size_ = 0;
};

// Streaming .gz replay. Carries a partial trailing frame over to the next chunk.
template <class H>
inline FrameStats replay_gz(const std::string& path, H& h, size_t chunk = 4u << 20) {
    gzFile f = gzopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open " + path);
    gzbuffer(f, 1u << 20);
    std::vector<std::byte> buf(chunk * 2);
    size_t have = 0;
    FrameStats st;
    for (;;) {
        const int n = gzread(f, buf.data() + have, static_cast<unsigned>(buf.size() - have));
        if (n < 0) {
            gzclose(f);
            throw std::runtime_error("gzread failed: " + path);
        }
        have += static_cast<size_t>(n);
        const size_t used = consume_frames(Bytes(buf.data(), have), h, st);
        std::memmove(buf.data(), buf.data() + used, have - used);
        have -= used;
        if (n == 0) break;
        if (have == buf.size()) buf.resize(buf.size() * 2);  // single frame larger than buffer
    }
    st.trailing_bytes = have;
    gzclose(f);
    return st;
}

template <class H>
inline FrameStats replay_mapped(const MappedFile& f, H& h) {
    FrameStats st;
    const size_t used = consume_frames(f.bytes(), h, st);
    st.trailing_bytes = f.bytes().size() - used;
    return st;
}

}  // namespace itch
