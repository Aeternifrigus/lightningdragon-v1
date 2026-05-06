#include "bloom.hpp"

#include <algorithm>
#include <utility>

namespace velocitydb {

namespace {

uint64_t fnv1a(std::string_view key) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : key) {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    return h;
}

}  // namespace

BloomFilter::BloomFilter(size_t expected_keys, size_t bits_per_key) {
    const size_t num_bits = std::max<size_t>(64, expected_keys * bits_per_key);
    bits_.assign((num_bits + 7) / 8, 0);
    // k = bits_per_key * ln 2 minimizes the false positive rate
    num_hashes_ = static_cast<uint32_t>(std::clamp<size_t>(bits_per_key * 69 / 100, 1, 30));
}

BloomFilter::BloomFilter(std::vector<uint8_t> bits, uint32_t num_hashes)
    : bits_(std::move(bits)), num_hashes_(num_hashes) {}

void BloomFilter::add(std::string_view key) {
    const uint64_t h1 = fnv1a(key);
    const uint64_t h2 = fnv1a(key) | 1;
    const uint64_t num_bits = bits_.size() * 8;
    for (uint32_t i = 0; i < num_hashes_; i++) {
        const uint64_t bit = (h1 + i * h2) % num_bits;
        bits_[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
    }
}

bool BloomFilter::may_contain(std::string_view key) const {
    if (bits_.empty()) return true;
    const uint64_t h1 = fnv1a(key);
    const uint64_t h2 = fnv1a(key) | 1;
    const uint64_t num_bits = bits_.size() * 8;
    for (uint32_t i = 0; i < num_hashes_; i++) {
        const uint64_t bit = (h1 + i * h2) % num_bits;
        if (!(bits_[bit / 8] & (1u << (bit % 8)))) return false;
    }
    return true;
}

}  // namespace velocitydb
