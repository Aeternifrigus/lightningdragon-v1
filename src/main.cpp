// Walks through the API and runs a small benchmark.
//
//   velocitydb_demo [writes] [reads] [threads]

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "compressor.hpp"
#include "velocitydb.hpp"

using namespace velocitydb;
using Clock = std::chrono::steady_clock;

namespace {

void section(const std::string& title) { std::cout << "\n== " << title << " ==\n"; }

std::string show(const std::optional<std::string>& v) { return v ? *v : "(none)"; }

std::string random_string(std::mt19937& gen, size_t length) {
    static const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::uniform_int_distribution<size_t> pick(0, sizeof(chars) - 2);
    std::string s(length, ' ');
    for (auto& c : s) c = chars[pick(gen)];
    return s;
}

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

void basic_operations(VelocityDB& db) {
    section("put / get / remove");
    db.put("user:1001", R"({"name": "Alice", "score": 9500})");
    db.put("user:1002", R"({"name": "Bob", "score": 8200})");
    std::cout << "user:1001 -> " << show(db.get("user:1001")) << "\n";
    db.remove("user:1002");
    std::cout << "user:1002 after remove -> " << show(db.get("user:1002")) << "\n";
}

void transactions(VelocityDB& db) {
    section("transactions");
    db.put("account:A", "1000");
    db.put("account:B", "500");

    auto txn = db.begin_transaction();
    const int a = std::stoi(*txn->get("account:A"));
    const int b = std::stoi(*txn->get("account:B"));
    txn->put("account:A", std::to_string(a - 200));
    txn->put("account:B", std::to_string(b + 200));
    std::cout << "transfer 200 A->B committed: " << (txn->commit() ? "yes" : "no") << "\n";
    std::cout << "A = " << show(db.get("account:A")) << ", B = " << show(db.get("account:B")) << "\n";

    auto t1 = db.begin_transaction();
    auto t2 = db.begin_transaction();
    t1->put("account:A", "0");
    t2->put("account:A", "1");
    const bool first = t1->commit();
    const bool second = t2->commit();
    std::cout << "two transactions write account:A: first commit " << (first ? "ok" : "failed")
              << ", second " << (second ? "ok" : "failed (conflict)") << "\n";
}

void snapshots(VelocityDB& db) {
    section("snapshots");
    db.put("counter", "100");
    const Snapshot snap = db.create_snapshot();
    db.put("counter", "200");
    std::cout << "current: " << show(db.get("counter")) << ", at snapshot: " << show(db.get("counter", snap))
              << "\n";
}

void compression() {
    section("compression");
    const std::string event =
        R"({"type":"event","ts":"2026-05-11T10:30:00Z","user_id":12345,"action":"page_view"})";
    std::mt19937 gen(7);

    struct Sample {
        const char* name;
        std::string data;
    };
    std::vector<Sample> samples = {{"repeated JSON events", ""}, {"random 100-byte values", ""}};
    for (int i = 0; i < 100; i++) samples[0].data += event;
    for (int i = 0; i < 100; i++) samples[1].data += random_string(gen, 100);

    for (const auto& s : samples) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(s.data.data());
        const auto packed = Compressor::compress(bytes, s.data.size());
        std::printf("%-24s %6zu -> %6zu bytes  (%.1fx)\n", s.name, s.data.size(), packed.size(),
                    static_cast<double>(s.data.size()) / packed.size());
    }
}

void benchmark(VelocityDB& db, size_t writes, size_t reads, size_t threads) {
    section("benchmark");
    const size_t per_thread = std::max<size_t>(1, writes / threads);
    const size_t total = per_thread * threads;

    std::vector<std::thread> workers;
    auto start = Clock::now();
    for (size_t t = 0; t < threads; t++) {
        workers.emplace_back([&db, t, per_thread] {
            std::mt19937 gen(static_cast<unsigned>(t));
            for (size_t i = 0; i < per_thread; i++) {
                db.put("bench:" + std::to_string(t) + ":" + std::to_string(i), random_string(gen, 100));
            }
        });
    }
    for (auto& w : workers) w.join();
    double elapsed = seconds_since(start);
    std::printf("writes: %zu in %.2fs on %zu threads = %.0f/s\n", total, elapsed, threads, total / elapsed);

    auto read_pass = [&](const char* label) {
        std::vector<std::vector<uint64_t>> latencies(threads);
        std::vector<std::thread> readers;
        const size_t reads_per_thread = std::max<size_t>(1, reads / threads);
        auto begin = Clock::now();
        for (size_t t = 0; t < threads; t++) {
            readers.emplace_back([&, t] {
                std::mt19937 gen(static_cast<unsigned>(100 + t));
                std::uniform_int_distribution<size_t> thread_pick(0, threads - 1);
                std::uniform_int_distribution<size_t> index_pick(0, per_thread - 1);
                latencies[t].reserve(reads_per_thread);
                for (size_t i = 0; i < reads_per_thread; i++) {
                    const std::string key = "bench:" + std::to_string(thread_pick(gen)) + ":" +
                                            std::to_string(index_pick(gen));
                    auto s = Clock::now();
                    db.get(key);
                    latencies[t].push_back(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - s).count());
                }
            });
        }
        for (auto& r : readers) r.join();
        const double secs = seconds_since(begin);

        std::vector<uint64_t> all;
        for (auto& l : latencies) all.insert(all.end(), l.begin(), l.end());
        std::sort(all.begin(), all.end());
        auto pct = [&](double p) { return all[std::min(all.size() - 1, size_t(p * all.size()))] / 1000.0; };
        std::printf("reads from %-9s %zu in %.2fs = %.0f/s  p50 %.1fus  p99 %.1fus  p99.9 %.1fus\n", label,
                    all.size(), secs, all.size() / secs, pct(0.50), pct(0.99), pct(0.999));
    };

    read_pass("memtable:");
    db.flush();
    read_pass("tables:");
}

void print_stats(const VelocityDB& db) {
    section("stats");
    const Stats& s = db.stats();
    std::printf("writes %llu, deletes %llu, reads %llu\n", (unsigned long long)s.writes.load(),
                (unsigned long long)s.deletes.load(), (unsigned long long)s.reads.load());
    std::printf("memtable hits %llu, table hits %llu, bloom skips %llu\n",
                (unsigned long long)s.memtable_hits.load(), (unsigned long long)s.table_hits.load(),
                (unsigned long long)s.bloom_skips.load());
    std::printf("flushes %llu, compactions %llu, tables %zu\n", (unsigned long long)s.flushes.load(),
                (unsigned long long)s.compactions.load(), db.table_count());
    if (s.table_bytes_stored.load() > 0) {
        std::printf("table data %llu bytes raw, %llu stored\n", (unsigned long long)s.table_bytes_raw.load(),
                    (unsigned long long)s.table_bytes_stored.load());
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    size_t writes = 100000;
    size_t reads = 100000;
    size_t threads = std::max(1u, std::thread::hardware_concurrency());
    if (argc > 1) writes = std::stoull(argv[1]);
    if (argc > 2) reads = std::stoull(argv[2]);
    if (argc > 3) threads = std::max<size_t>(1, std::stoull(argv[3]));

    Config config;
    config.data_dir = "./velocitydb_demo_data";
    config.memtable_size_limit = 32 * 1024 * 1024;
    std::filesystem::remove_all(config.data_dir);

    VelocityDB db(config);
    basic_operations(db);
    transactions(db);
    snapshots(db);
    compression();
    benchmark(db, writes, reads, threads);
    print_stats(db);
    return 0;
}
