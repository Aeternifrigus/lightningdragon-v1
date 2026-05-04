#include "compressor.hpp"

#include <array>
#include <stdexcept>
#include <unordered_map>

namespace velocitydb {

namespace {

constexpr uint8_t kMatch = 0xFF;
constexpr uint8_t kRun = 0xFE;
constexpr uint8_t kEscape = 0xFD;
constexpr size_t kMinMatch = 4;
constexpr size_t kMaxLength = 255;
constexpr size_t kMaxOffset = 65535;
constexpr size_t kCandidates = 8;  // positions remembered per hash

uint32_t read4(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

struct Candidates {
    std::array<size_t, kCandidates> pos{};
    size_t count = 0;

    void add(size_t p) { pos[count++ % kCandidates] = p; }
    size_t size() const { return count < kCandidates ? count : kCandidates; }
};

}  // namespace

std::vector<uint8_t> Compressor::compress(const uint8_t* data, size_t size) {
    if (size > UINT32_MAX) throw std::length_error("compress: input larger than 4 GiB");

    std::vector<uint8_t> out;
    out.reserve(size / 2 + 16);
    out.push_back(static_cast<uint8_t>(size >> 24));
    out.push_back(static_cast<uint8_t>(size >> 16));
    out.push_back(static_cast<uint8_t>(size >> 8));
    out.push_back(static_cast<uint8_t>(size));

    std::unordered_map<uint32_t, Candidates> seen;

    size_t i = 0;
    while (i < size) {
        size_t best_len = 0;
        size_t best_off = 0;

        if (i + kMinMatch <= size) {
            const uint32_t h = read4(data + i);
            Candidates& c = seen[h];
            for (size_t k = 0; k < c.size(); k++) {
                const size_t pos = c.pos[k];
                if (i - pos > kMaxOffset) continue;
                size_t len = 0;
                while (i + len < size && len < kMaxLength && data[pos + len] == data[i + len]) len++;
                if (len > best_len) {
                    best_len = len;
                    best_off = i - pos;
                }
            }
            c.add(i);
        }

        if (best_len >= kMinMatch) {
            out.push_back(kMatch);
            out.push_back(static_cast<uint8_t>(best_len));
            out.push_back(static_cast<uint8_t>(best_off >> 8));
            out.push_back(static_cast<uint8_t>(best_off & 0xFF));
            i += best_len;
            continue;
        }

        size_t run = 1;
        while (i + run < size && run < kMaxLength && data[i + run] == data[i]) run++;
        if (run >= 4) {
            out.push_back(kRun);
            out.push_back(static_cast<uint8_t>(run));
            out.push_back(data[i]);
            i += run;
            continue;
        }

        if (data[i] >= kEscape) out.push_back(kEscape);
        out.push_back(data[i]);
        i++;
    }
    return out;
}

std::optional<std::vector<uint8_t>> Compressor::decompress(const uint8_t* data, size_t size) {
    if (size < 4) return std::nullopt;
    const size_t expected = read4(data);

    std::vector<uint8_t> out;
    out.reserve(expected);

    size_t i = 4;
    while (i < size) {
        const uint8_t tag = data[i];
        if (tag == kMatch) {
            if (i + 3 >= size) return std::nullopt;
            const size_t len = data[i + 1];
            const size_t off = (static_cast<size_t>(data[i + 2]) << 8) | data[i + 3];
            if (off == 0 || off > out.size()) return std::nullopt;
            const size_t from = out.size() - off;
            for (size_t k = 0; k < len; k++) out.push_back(out[from + k]);
            i += 4;
        } else if (tag == kRun) {
            if (i + 2 >= size) return std::nullopt;
            out.insert(out.end(), data[i + 1], data[i + 2]);
            i += 3;
        } else if (tag == kEscape) {
            if (i + 1 >= size) return std::nullopt;
            out.push_back(data[i + 1]);
            i += 2;
        } else {
            out.push_back(tag);
            i++;
        }
        if (out.size() > expected) return std::nullopt;
    }

    if (out.size() != expected) return std::nullopt;
    return out;
}

}  // namespace velocitydb
