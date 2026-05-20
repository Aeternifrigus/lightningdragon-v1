#include <string>

#include "bloom.hpp"
#include "check.hpp"

using velocitydb::BloomFilter;

TEST(bloom_has_no_false_negatives) {
    BloomFilter bloom(10000, 10);
    for (int i = 0; i < 10000; i++) bloom.add("key" + std::to_string(i));
    int missing = 0;
    for (int i = 0; i < 10000; i++) {
        if (!bloom.may_contain("key" + std::to_string(i))) missing++;
    }
    CHECK_EQ(missing, 0);
}

TEST(bloom_false_positive_rate) {
    BloomFilter bloom(10000, 10);
    for (int i = 0; i < 10000; i++) bloom.add("key" + std::to_string(i));
    int false_positives = 0;
    for (int i = 0; i < 100000; i++) {
        if (bloom.may_contain("other" + std::to_string(i))) false_positives++;
    }
    // about 1% is expected at 10 bits per key
    CHECK(false_positives < 2000);
}

TEST(bloom_rebuilt_from_bits_matches) {
    BloomFilter bloom(100, 10);
    for (int i = 0; i < 100; i++) bloom.add("k" + std::to_string(i));
    BloomFilter copy(bloom.bits(), bloom.num_hashes());
    for (int i = 0; i < 100; i++) CHECK(copy.may_contain("k" + std::to_string(i)));
    for (int i = 0; i < 1000; i++) {
        const std::string key = "x" + std::to_string(i);
        CHECK_EQ(copy.may_contain(key), bloom.may_contain(key));
    }
}
