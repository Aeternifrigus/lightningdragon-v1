#include <atomic>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "check.hpp"
#include "velocitydb.hpp"

using velocitydb::Config;
using velocitydb::VelocityDB;
using velocitydb::WriteBatch;

namespace {

Config test_config(const check::TempDir& dir) {
    Config c;
    c.data_dir = dir.str();
    c.background_maintenance = false;  // tests flush and compact explicitly
    return c;
}

std::string value_of(const std::optional<std::string>& v) { return v ? *v : "<none>"; }

}  // namespace

TEST(db_put_get_remove) {
    check::TempDir dir;
    VelocityDB db(test_config(dir));
    db.put("a", "1");
    db.put("b", "2");
    CHECK_EQ(value_of(db.get("a")), std::string("1"));
    db.put("a", "3");
    CHECK_EQ(value_of(db.get("a")), std::string("3"));
    db.remove("b");
    CHECK(!db.get("b"));
    CHECK(!db.get("missing"));
}

TEST(db_reads_after_flush) {
    check::TempDir dir;
    VelocityDB db(test_config(dir));
    for (int i = 0; i < 1000; i++) db.put("key" + std::to_string(i), "v" + std::to_string(i));
    db.flush();
    CHECK_EQ(db.table_count(), 1u);
    CHECK_EQ(value_of(db.get("key0")), std::string("v0"));
    CHECK_EQ(value_of(db.get("key999")), std::string("v999"));
    CHECK(!db.get("key1000"));
}

TEST(db_newest_table_wins) {
    check::TempDir dir;
    VelocityDB db(test_config(dir));
    db.put("k", "old");
    db.flush();
    db.put("k", "new");
    db.flush();
    CHECK_EQ(db.table_count(), 2u);
    CHECK_EQ(value_of(db.get("k")), std::string("new"));
}

TEST(db_delete_hides_value_in_older_table) {
    check::TempDir dir;
    VelocityDB db(test_config(dir));
    db.put("k", "v");
    db.flush();
    db.remove("k");
    CHECK(!db.get("k"));  // tombstone in the memtable
    db.flush();
    CHECK(!db.get("k"));  // tombstone in the newer table
}

TEST(db_data_survives_reopen) {
    check::TempDir dir;
    {
        VelocityDB db(test_config(dir));
        db.put("a", "1");
        db.put("b", "2");
        db.flush();
        db.put("b", "3");
        db.remove("a");
    }  // closing flushes the memtable
    VelocityDB db(test_config(dir));
    CHECK(!db.get("a"));
    CHECK_EQ(value_of(db.get("b")), std::string("3"));
    db.put("c", "4");  // sequence numbers carry on from the stored ones
    CHECK_EQ(value_of(db.get("c")), std::string("4"));
}

TEST(db_write_batch) {
    check::TempDir dir;
    VelocityDB db(test_config(dir));
    db.put("x", "old");
    WriteBatch batch;
    batch.put("x", "new");
    batch.put("y", "1");
    batch.remove("z");
    db.write(batch);
    CHECK_EQ(value_of(db.get("x")), std::string("new"));
    CHECK_EQ(value_of(db.get("y")), std::string("1"));
}

TEST(db_snapshot_reads) {
    check::TempDir dir;
    VelocityDB db(test_config(dir));
    db.put("counter", "100");
    auto snap = db.create_snapshot();
    db.put("counter", "200");
    db.put("new_key", "x");
    db.remove("counter");
    CHECK_EQ(value_of(db.get("counter", snap)), std::string("100"));
    CHECK(!db.get("new_key", snap));
    CHECK(!db.get("counter"));
}
