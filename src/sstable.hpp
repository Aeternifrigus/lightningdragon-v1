#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bloom.hpp"
#include "skiplist.hpp"

namespace velocitydb {

// Immutable sorted table on disk.
//
//   data    entries, compressed or raw:
//           u8 deleted | u64 sequence | u32 key length | u32 value length | key | value
//   index   per entry: u32 key length | key | u64 offset into the raw data
//   bloom   filter bits
//   footer  12 x u64, see Footer
//
// The whole table is loaded into memory when it is opened; lookups do a binary
// search over the index and never touch the file.
class SSTable {
public:
    struct Entry {
        std::string key;
        std::string value;
        uint64_t sequence = 0;
        bool deleted = false;
    };

    struct Options {
        bool compress = true;
        size_t bloom_bits_per_key = 10;
    };

    // Entries must be sorted by key with no duplicates. Writes to a temporary
    // file, fsyncs it and renames it into place.
    static std::shared_ptr<SSTable> write(const std::string& path, const std::vector<Entry>& entries,
                                          const Options& options);
    // Throws std::runtime_error if the file is damaged.
    static std::shared_ptr<SSTable> open(const std::string& path);

    bool may_contain(std::string_view key) const { return bloom_.may_contain(key); }
    // Only the newest version of each key is stored, so a snapshot older than
    // that version gets NotFound.
    LookupResult get(std::string_view key, uint64_t snapshot = UINT64_MAX) const;
    std::vector<Entry> entries() const;

    const std::string& path() const { return path_; }
    size_t size() const { return index_.size(); }
    uint64_t max_sequence() const { return max_sequence_; }
    uint64_t raw_bytes() const { return data_.size(); }
    uint64_t stored_bytes() const { return stored_bytes_; }

private:
    SSTable(std::string path, BloomFilter bloom) : path_(std::move(path)), bloom_(std::move(bloom)) {}
    Entry decode(uint64_t offset) const;

    std::string path_;
    BloomFilter bloom_;
    std::vector<uint8_t> data_;  // uncompressed
    std::vector<std::pair<std::string, uint64_t>> index_;
    uint64_t max_sequence_ = 0;
    uint64_t stored_bytes_ = 0;
};

}  // namespace velocitydb
