#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "check.hpp"
#include "sstable.hpp"

using velocitydb::LookupResult;
using velocitydb::SSTable;

namespace {

std::vector<SSTable::Entry> sample_entries(size_t n) {
    std::vector<SSTable::Entry> entries;
    for (size_t i = 0; i < n; i++) {
        char key[32];
        std::snprintf(key, sizeof(key), "key%06zu", i);
        entries.push_back({key, "value-" + std::to_string(i), i + 1, i % 10 == 9});
    }
    return entries;
}

}  // namespace

TEST(sstable_write_then_get) {
    check::TempDir dir;
    const std::string path = (dir.path() / "1.sst").string();
    auto table = SSTable::write(path, sample_entries(1000), {});
    CHECK_EQ(table->size(), 1000u);
    CHECK_EQ(table->get("key000005").value, std::string("value-5"));
    CHECK(table->get("key000009").state == LookupResult::State::Deleted);
    CHECK(table->get("key999999").state == LookupResult::State::NotFound);
    CHECK(table->get("a").state == LookupResult::State::NotFound);
}

TEST(sstable_reopen_reads_same_data) {
    check::TempDir dir;
    const std::string path = (dir.path() / "1.sst").string();
    const auto entries = sample_entries(1000);
    SSTable::write(path, entries, {});

    auto table = SSTable::open(path);
    CHECK_EQ(table->size(), entries.size());
    CHECK_EQ(table->max_sequence(), 1000u);
    for (const auto& e : entries) {
        CHECK(table->may_contain(e.key));
        const auto r = table->get(e.key);
        if (e.deleted) {
            CHECK(r.state == LookupResult::State::Deleted);
        } else {
            CHECK_EQ(r.value, e.value);
        }
    }
    CHECK_EQ(table->entries().size(), entries.size());
}

TEST(sstable_uncompressed_and_incompressible) {
    check::TempDir dir;
    std::mt19937 gen(1);
    std::vector<SSTable::Entry> entries;
    for (int i = 0; i < 200; i++) {
        std::string value(64, ' ');
        for (auto& c : value) c = static_cast<char>(gen());
        entries.push_back({"k" + std::to_string(1000 + i), value, static_cast<uint64_t>(i + 1), false});
    }
    for (bool compress : {true, false}) {
        const std::string path = (dir.path() / (compress ? "c.sst" : "u.sst")).string();
        auto written = SSTable::write(path, entries, {compress, 10});
        CHECK(written->stored_bytes() <= written->raw_bytes());
        auto table = SSTable::open(path);
        CHECK_EQ(table->get("k1100").value, entries[100].value);
    }

    // one large random value: compressing it would make it bigger
    std::string blob(65536, ' ');
    for (auto& c : blob) c = static_cast<char>(gen());
    const std::string path = (dir.path() / "blob.sst").string();
    auto written = SSTable::write(path, {{"blob", blob, 1, false}}, {true, 10});
    CHECK(written->stored_bytes() <= written->raw_bytes());
    CHECK_EQ(SSTable::open(path)->get("blob").value, blob);
}

TEST(sstable_snapshot_filter) {
    check::TempDir dir;
    auto table = SSTable::write((dir.path() / "1.sst").string(), {{"a", "1", 10, false}}, {});
    CHECK(table->get("a", 10).state == LookupResult::State::Found);
    CHECK(table->get("a", 9).state == LookupResult::State::NotFound);
}

TEST(sstable_rejects_unsorted_input) {
    check::TempDir dir;
    bool threw = false;
    try {
        SSTable::write((dir.path() / "1.sst").string(), {{"b", "", 1, false}, {"a", "", 2, false}}, {});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}
