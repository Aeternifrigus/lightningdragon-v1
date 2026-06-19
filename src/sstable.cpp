#include "sstable.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>

#include <unistd.h>

#include "coding.hpp"
#include "compressor.hpp"

namespace velocitydb {

namespace {

constexpr uint64_t kMagic = 0x5654444231534254ULL;  // "VTDB1SBT"
constexpr uint64_t kFlagCompressed = 1;
constexpr size_t kFooterFields = 12;
constexpr size_t kFooterSize = kFooterFields * 8;

enum FooterField {
    kFieldMagic,
    kFieldFlags,
    kFieldEntries,
    kFieldRawSize,
    kFieldDataSize,
    kFieldIndexSize,
    kFieldBloomSize,
    kFieldBloomHashes,
    kFieldMinSequence,
    kFieldMaxSequence,
    kFieldReserved,
    kFieldReserved2,
};

[[noreturn]] void corrupt(const std::string& path, const std::string& what) {
    throw std::runtime_error("corrupt table " + path + ": " + what);
}

void write_file(const std::string& path, const std::string& contents) {
    const std::string tmp = path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) throw std::system_error(errno, std::generic_category(), "open " + tmp);
    const bool ok = std::fwrite(contents.data(), 1, contents.size(), f) == contents.size() &&
                    std::fflush(f) == 0 && ::fsync(fileno(f)) == 0;
    const int err = errno;
    std::fclose(f);
    if (!ok) {
        std::remove(tmp.c_str());
        throw std::system_error(err, std::generic_category(), "write " + tmp);
    }
    std::filesystem::rename(tmp, path);
}

}  // namespace

std::shared_ptr<SSTable> SSTable::write(const std::string& path, const std::vector<Entry>& entries,
                                        const Options& options) {
    BloomFilter bloom(entries.size(), options.bloom_bits_per_key);
    std::shared_ptr<SSTable> table(new SSTable(path, BloomFilter(0, 1)));

    std::string raw;
    std::string index;
    uint64_t min_seq = UINT64_MAX;
    uint64_t max_seq = 0;
    for (size_t i = 0; i < entries.size(); i++) {
        const Entry& e = entries[i];
        if (i > 0 && !(entries[i - 1].key < e.key)) {
            throw std::invalid_argument("SSTable::write: entries must be sorted and unique");
        }
        bloom.add(e.key);
        min_seq = std::min(min_seq, e.sequence);
        max_seq = std::max(max_seq, e.sequence);

        table->index_.emplace_back(e.key, raw.size());
        put_u32(index, static_cast<uint32_t>(e.key.size()));
        index += e.key;
        put_u64(index, raw.size());

        raw.push_back(e.deleted ? 1 : 0);
        put_u64(raw, e.sequence);
        put_u32(raw, static_cast<uint32_t>(e.key.size()));
        put_u32(raw, static_cast<uint32_t>(e.value.size()));
        raw += e.key;
        raw += e.value;
    }

    const auto* raw_bytes = reinterpret_cast<const uint8_t*>(raw.data());
    std::string data;
    uint64_t flags = 0;
    if (options.compress && !raw.empty()) {
        const auto compressed = Compressor::compress(raw_bytes, raw.size());
        // keep the raw bytes when compression doesn't help (random values)
        if (compressed.size() < raw.size()) {
            data.assign(compressed.begin(), compressed.end());
            flags |= kFlagCompressed;
        }
    }
    if (!(flags & kFlagCompressed)) data = raw;

    const auto& bits = bloom.bits();
    std::string file = data + index;
    file.append(reinterpret_cast<const char*>(bits.data()), bits.size());

    uint64_t footer[kFooterFields] = {};
    footer[kFieldMagic] = kMagic;
    footer[kFieldFlags] = flags;
    footer[kFieldEntries] = entries.size();
    footer[kFieldRawSize] = raw.size();
    footer[kFieldDataSize] = data.size();
    footer[kFieldIndexSize] = index.size();
    footer[kFieldBloomSize] = bits.size();
    footer[kFieldBloomHashes] = bloom.num_hashes();
    footer[kFieldMinSequence] = entries.empty() ? 0 : min_seq;
    footer[kFieldMaxSequence] = max_seq;
    for (uint64_t field : footer) put_u64(file, field);

    write_file(path, file);

    table->bloom_ = std::move(bloom);
    table->data_.assign(raw_bytes, raw_bytes + raw.size());
    table->max_sequence_ = max_seq;
    table->stored_bytes_ = data.size();
    return table;
}

std::shared_ptr<SSTable> SSTable::open(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::system_error(errno, std::generic_category(), "open " + path);
    const std::string buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (buf.size() < kFooterSize) corrupt(path, "too small");

    const auto* bytes = reinterpret_cast<const uint8_t*>(buf.data());
    uint64_t footer[kFooterFields];
    for (size_t i = 0; i < kFooterFields; i++) {
        footer[i] = get_u64(bytes + buf.size() - kFooterSize + i * 8);
    }
    if (footer[kFieldMagic] != kMagic) corrupt(path, "bad magic");

    const uint64_t data_size = footer[kFieldDataSize];
    const uint64_t index_size = footer[kFieldIndexSize];
    const uint64_t bloom_size = footer[kFieldBloomSize];
    if (data_size + index_size + bloom_size + kFooterSize != buf.size()) {
        corrupt(path, "section sizes don't add up");
    }

    const uint8_t* bloom_start = bytes + data_size + index_size;
    BloomFilter bloom(std::vector<uint8_t>(bloom_start, bloom_start + bloom_size),
                      static_cast<uint32_t>(footer[kFieldBloomHashes]));
    std::shared_ptr<SSTable> table(new SSTable(path, std::move(bloom)));

    if (footer[kFieldFlags] & kFlagCompressed) {
        auto raw = Compressor::decompress(bytes, data_size);
        if (!raw) corrupt(path, "data does not decompress");
        table->data_ = std::move(*raw);
    } else {
        table->data_.assign(bytes, bytes + data_size);
    }
    if (table->data_.size() != footer[kFieldRawSize]) corrupt(path, "raw size mismatch");

    const uint8_t* p = bytes + data_size;
    const uint8_t* end = p + index_size;
    table->index_.reserve(footer[kFieldEntries]);
    while (p < end) {
        if (end - p < 4) corrupt(path, "truncated index");
        const uint32_t klen = get_u32(p);
        if (static_cast<uint64_t>(end - p) < 4ULL + klen + 8) corrupt(path, "truncated index");
        std::string key(reinterpret_cast<const char*>(p + 4), klen);
        const uint64_t offset = get_u64(p + 4 + klen);
        if (offset >= table->data_.size()) corrupt(path, "index points past data");
        table->index_.emplace_back(std::move(key), offset);
        p += 4 + klen + 8;
    }
    if (table->index_.size() != footer[kFieldEntries]) corrupt(path, "entry count mismatch");

    table->max_sequence_ = footer[kFieldMaxSequence];
    table->stored_bytes_ = data_size;
    return table;
}

SSTable::Entry SSTable::decode(uint64_t offset) const {
    const uint8_t* p = data_.data() + offset;
    const size_t remaining = data_.size() - offset;
    if (remaining < 17) corrupt(path_, "truncated entry");
    Entry e;
    e.deleted = p[0] != 0;
    e.sequence = get_u64(p + 1);
    const uint32_t klen = get_u32(p + 9);
    const uint32_t vlen = get_u32(p + 13);
    if (remaining < 17ULL + klen + vlen) corrupt(path_, "truncated entry");
    e.key.assign(reinterpret_cast<const char*>(p + 17), klen);
    e.value.assign(reinterpret_cast<const char*>(p + 17 + klen), vlen);
    return e;
}

LookupResult SSTable::get(std::string_view key, uint64_t snapshot) const {
    LookupResult result;
    auto it = std::lower_bound(index_.begin(), index_.end(), key,
                               [](const auto& entry, std::string_view k) { return entry.first < k; });
    if (it == index_.end() || it->first != key) return result;

    Entry e = decode(it->second);
    if (e.sequence > snapshot) return result;
    result.state = e.deleted ? LookupResult::State::Deleted : LookupResult::State::Found;
    result.sequence = e.sequence;
    result.value = std::move(e.value);
    return result;
}

std::vector<SSTable::Entry> SSTable::entries() const {
    std::vector<Entry> out;
    out.reserve(index_.size());
    for (const auto& [key, offset] : index_) out.push_back(decode(offset));
    return out;
}

}  // namespace velocitydb
