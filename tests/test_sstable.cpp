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
        const auto r = table->get(e.key);
        if (e.deleted) {
            CHECK(r.state == LookupResult::State::Deleted);
        } else {
            CHECK_EQ(r.value, e.value);
        }
    }
    CHECK_EQ(table->entries().size(), entries.size());
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
