#include <algorithm>
#include <string>
#include <thread>
#include <vector>

#include "check.hpp"
#include "skiplist.hpp"

using velocitydb::LookupResult;
using velocitydb::SkipList;

TEST(skiplist_put_get) {
    SkipList list;
    list.put("b", "2", 1);
    list.put("a", "1", 2);
    CHECK(list.get("a").state == LookupResult::State::Found);
    CHECK_EQ(list.get("a").value, std::string("1"));
    CHECK_EQ(list.get("b").value, std::string("2"));
    CHECK(list.get("c").state == LookupResult::State::NotFound);
    CHECK_EQ(list.key_count(), 2u);
}

TEST(skiplist_overwrite_keeps_versions) {
    SkipList list;
    list.put("k", "v1", 1);
    list.put("k", "v2", 5);
    CHECK_EQ(list.get("k").value, std::string("v2"));
    CHECK_EQ(list.get("k", 4).value, std::string("v1"));
    CHECK(list.get("k", 0).state == LookupResult::State::NotFound);
    CHECK_EQ(list.key_count(), 1u);
}

TEST(skiplist_versions_sorted_when_inserted_out_of_order) {
    SkipList list;
    list.put("k", "v3", 3);
    list.put("k", "v1", 1);
    list.put("k", "v2", 2);
    CHECK_EQ(list.get("k").value, std::string("v3"));
    CHECK_EQ(list.get("k", 2).value, std::string("v2"));
    CHECK_EQ(list.get("k", 1).value, std::string("v1"));
}

TEST(skiplist_remove_is_a_tombstone) {
    SkipList list;
    list.put("k", "v", 1);
    list.remove("k", 2);
    CHECK(list.get("k").state == LookupResult::State::Deleted);
    CHECK_EQ(list.get("k", 1).value, std::string("v"));
    // deleting a key the list has never seen still records the delete
    list.remove("other", 3);
    CHECK(list.get("other").state == LookupResult::State::Deleted);
}

TEST(skiplist_iterates_in_key_order) {
    SkipList list;
    std::vector<std::string> keys;
    for (int i = 0; i < 500; i++) {
        keys.push_back("key" + std::to_string((i * 7919) % 1000));
        list.put(keys.back(), "v", static_cast<uint64_t>(i + 1));
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());

    std::vector<std::string> seen;
    for (auto it = list.begin(); it.valid(); it.next()) seen.push_back(it.key());
    CHECK(seen == keys);
}

TEST(skiplist_concurrent_inserts) {
    SkipList list;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 2000;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++) {
        threads.emplace_back([&list, t] {
            for (int i = 0; i < kPerThread; i++) {
                // half the keys are shared between threads
                const std::string key = (i % 2) ? "shared" + std::to_string(i)
                                                : "t" + std::to_string(t) + "_" + std::to_string(i);
                list.put(key, "v", static_cast<uint64_t>(t * kPerThread + i + 1));
            }
        });
    }
    for (auto& th : threads) th.join();

    CHECK_EQ(list.key_count(), static_cast<size_t>(kThreads * kPerThread / 2 + kPerThread / 2));
    size_t count = 0;
    std::string prev;
    for (auto it = list.begin(); it.valid(); it.next()) {
        CHECK(count == 0 || prev < it.key());
        prev = it.key();
        count++;
    }
    CHECK_EQ(count, list.key_count());
}
