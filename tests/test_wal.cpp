#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "check.hpp"
#include "wal.hpp"

using velocitydb::WAL;
using velocitydb::WriteOp;

namespace {

struct Replayed {
    uint64_t sequence;
    WriteOp op;
};

std::vector<Replayed> replay_all(const std::string& path) {
    std::vector<Replayed> out;
    WAL::replay(path, [&](uint64_t seq, const WriteOp& op) { out.push_back({seq, op}); });
    return out;
}

}  // namespace

TEST(wal_replays_batches_in_order) {
    check::TempDir dir;
    const std::string path = (dir.path() / "wal.log").string();
    {
        WAL wal(path);
        wal.append(1, {{"a", "1", false}});
        wal.append(2, {{"b", "2", false}, {"a", "", true}, {"c", std::string(1000, 'x'), false}});
    }
    auto ops = replay_all(path);
    CHECK_EQ(ops.size(), 4u);
    if (ops.size() == 4) {
        CHECK_EQ(ops[0].sequence, 1u);
        CHECK_EQ(ops[0].op.key, std::string("a"));
        CHECK_EQ(ops[1].sequence, 2u);
        CHECK_EQ(ops[2].sequence, 3u);
        CHECK(ops[2].op.deleted);
        CHECK_EQ(ops[3].sequence, 4u);
        CHECK_EQ(ops[3].op.value.size(), 1000u);
    }
}

TEST(wal_missing_file_replays_nothing) {
    check::TempDir dir;
    CHECK(replay_all((dir.path() / "nope.log").string()).empty());
}

TEST(wal_stops_at_torn_record) {
    check::TempDir dir;
    const std::string path = (dir.path() / "wal.log").string();
    {
        WAL wal(path);
        wal.append(1, {{"a", "1", false}});
        wal.append(2, {{"b", "2", false}, {"c", "3", false}});
    }
    // chop the last few bytes off, as a crash mid-write would
    std::filesystem::resize_file(path, std::filesystem::file_size(path) - 3);
    auto ops = replay_all(path);
    CHECK_EQ(ops.size(), 1u);  // the second batch is dropped whole, not half-applied
}

TEST(wal_stops_at_corrupt_record) {
    check::TempDir dir;
    const std::string path = (dir.path() / "wal.log").string();
    {
        WAL wal(path);
        wal.append(1, {{"a", "1", false}});
        wal.append(2, {{"b", "2", false}});
        wal.append(3, {{"c", "3", false}});
    }
    // flip a byte inside the second record's payload
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    const auto first_record = 8 + 12 + 9 + 2;
    f.seekp(first_record + 8 + 14);
    f.put('Z');
    f.close();
    CHECK_EQ(replay_all(path).size(), 1u);
}

TEST(wal_rotate_starts_a_new_log) {
    check::TempDir dir;
    const std::string path = (dir.path() / "wal.log").string();
    WAL wal(path);
    wal.append(1, {{"a", "1", false}});
    const std::string old = wal.rotate();
    wal.append(2, {{"b", "2", false}});
    wal.sync();

    auto old_ops = replay_all(old);
    auto new_ops = replay_all(path);
    CHECK_EQ(old_ops.size(), 1u);
    CHECK_EQ(new_ops.size(), 1u);
    if (!new_ops.empty()) CHECK_EQ(new_ops[0].op.key, std::string("b"));
}
