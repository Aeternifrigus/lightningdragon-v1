#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace velocitydb {

// Bloom filter with double hashing: bit i = h1 + i * h2.
class BloomFilter {
public:
    BloomFilter(size_t expected_keys, size_t bits_per_key);
    // Rebuild a filter read back from a table file.
    BloomFilter(std::vector<uint8_t> bits, uint32_t num_hashes);

    void add(std::string_view key);
    bool may_contain(std::string_view key) const;

    const std::vector<uint8_t>& bits() const { return bits_; }
    uint32_t num_hashes() const { return num_hashes_; }

private:
    std::vector<uint8_t> bits_;
    uint32_t num_hashes_;
};

}  // namespace velocitydb
