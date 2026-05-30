#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "check.hpp"
#include "velocitydb.hpp"

using velocitydb::Config;
using velocitydb::VelocityDB;

namespace {

Config test_config(const check::TempDir& dir) {
    Config c;
    c.data_dir = dir.str();
    c.background_maintenance = false;
    return c;
}

}  // namespace

TEST(txn_commit_applies_all_writes) {
    check::TempDir dir;
    VelocityDB db(test_config(dir));
    db.put("a", "1");
    auto txn = db.begin_transaction();
    txn->put("a", "2");
    txn->put("b", "3");
    txn->remove("c");
    CHECK(txn->commit());
    CHECK_EQ(*db.get("a"), std::string("2"));
    CHECK_EQ(*db.get("b"), std::string("3"));
    CHECK(!txn->commit());  // only once
}

TEST(txn_reads_own_writes_and_snapshot) {
    check::TempDir dir;
    VelocityDB db(test_config(dir));
    db.put("a", "1");
    auto txn = db.begin_transaction();
    db.put("a", "changed outside");
    CHECK_EQ(*txn->get("a"), std::string("1"));
    txn->put("a", "mine");
    CHECK_EQ(*txn->get("a"), std::string("mine"));
    txn->remove("a");
    CHECK(!txn->get("a"));
}

TEST(txn_rollback_discards) {
    check::TempDir dir;
    VelocityDB db(test_config(dir));
    {
        auto txn = db.begin_transaction();
        txn->put("a", "1");
        txn->rollback();
        CHECK(!txn->commit());
    }
    {
        auto txn = db.begin_transaction();
        txn->put("b", "1");
    }  // dropped without commit
    CHECK(!db.get("a"));
    CHECK(!db.get("b"));
}

TEST(txn_write_conflict_fails_second_commit) {
    check::TempDir dir;
    VelocityDB db(test_config(dir));
    db.put("balance", "100");
    auto t1 = db.begin_transaction();
    auto t2 = db.begin_transaction();
    t1->put("balance", "90");
    t2->put("balance", "80");
    CHECK(t1->commit());
    CHECK(!t2->commit());
    CHECK_EQ(*db.get("balance"), std::string("90"));
}

TEST(txn_conflict_with_plain_write) {
    check::TempDir dir;
    VelocityDB db(test_config(dir));
    auto txn = db.begin_transaction();
    txn->put("k", "from txn");
    db.put("k", "direct");
    CHECK(!txn->commit());
    CHECK_EQ(*db.get("k"), std::string("direct"));
}

TEST(txn_conflict_detected_after_flush) {
    check::TempDir dir;
    VelocityDB db(test_config(dir));
    auto txn = db.begin_transaction();
    db.put("k", "direct");
    db.flush();
    txn->put("k", "from txn");
    CHECK(!txn->commit());
}

TEST(txn_disjoint_keys_both_commit) {
    check::TempDir dir;
    VelocityDB db(test_config(dir));
    auto t1 = db.begin_transaction();
    auto t2 = db.begin_transaction();
    t1->put("x", "1");
    t2->put("y", "2");
    CHECK(t1->commit());
    CHECK(t2->commit());
}

TEST(txn_commits_are_atomic_to_readers) {
    check::TempDir dir;
    VelocityDB db(test_config(dir));
    db.put("A", "1000");
    db.put("B", "0");

    std::atomic<bool> done{false};
    std::atomic<int> torn{0};
    std::thread reader([&] {
        while (!done.load()) {
            auto snap = db.create_snapshot();
            const int a = std::stoi(*db.get("A", snap));
            const int b = std::stoi(*db.get("B", snap));
            if (a + b != 1000) torn++;
        }
    });

    int committed = 0;
    for (int i = 0; i < 2000; i++) {
        auto txn = db.begin_transaction();
        const int a = std::stoi(*txn->get("A"));
        const int b = std::stoi(*txn->get("B"));
        txn->put("A", std::to_string(a - 1));
        txn->put("B", std::to_string(b + 1));
        if (txn->commit()) committed++;
    }
    done = true;
    reader.join();

    CHECK_EQ(torn.load(), 0);
    CHECK_EQ(committed, 2000);
    CHECK_EQ(*db.get("B"), std::string("2000"));
}
