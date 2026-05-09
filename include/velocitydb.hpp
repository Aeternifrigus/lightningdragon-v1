#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace velocitydb {

class SkipList;
class SSTable;
class WAL;
class VelocityDB;
struct LookupResult;

struct Config {
    std::string data_dir = "./velocitydb_data";
    size_t memtable_size_limit = 64 * 1024 * 1024;  // flush once the memtable is this big
    size_t bloom_bits_per_key = 10;
    bool enable_compression = true;
    bool enable_wal = true;
    bool background_maintenance = true;  // flush on a background thread
};

struct Stats {
    std::atomic<uint64_t> writes{0};
    std::atomic<uint64_t> deletes{0};
    std::atomic<uint64_t> reads{0};
    std::atomic<uint64_t> memtable_hits{0};
    std::atomic<uint64_t> table_hits{0};
    std::atomic<uint64_t> bloom_skips{0};  // table lookups avoided by a bloom filter
    std::atomic<uint64_t> flushes{0};
    std::atomic<uint64_t> table_bytes_raw{0};     // table data before compression
    std::atomic<uint64_t> table_bytes_stored{0};  // and after
};

struct Snapshot {
    uint64_t sequence = 0;
};

struct WriteOp {
    std::string key;
    std::string value;
    bool deleted = false;
};

// A group of writes that is applied atomically: readers see all of it or none.
class WriteBatch {
public:
    void put(std::string_view key, std::string_view value);
    void remove(std::string_view key);

    const std::vector<WriteOp>& ops() const { return ops_; }
    size_t size() const { return ops_.size(); }
    bool empty() const { return ops_.empty(); }
    void clear() { ops_.clear(); }

private:
    std::vector<WriteOp> ops_;
};

class VelocityDB {
public:
    explicit VelocityDB(Config config = Config{});
    ~VelocityDB();

    VelocityDB(const VelocityDB&) = delete;
    VelocityDB& operator=(const VelocityDB&) = delete;

    void put(std::string_view key, std::string_view value);
    void remove(std::string_view key);
    void write(const WriteBatch& batch);

    std::optional<std::string> get(std::string_view key) const;
    std::optional<std::string> get(std::string_view key, const Snapshot& snapshot) const;

    // A snapshot pins the current state for reads. Nothing needs releasing, but
    // see the README: versions overwritten before a flush are not kept on disk.
    Snapshot create_snapshot() const;

    // Write the memtable to a table file and sync the WAL.
    void flush();

    const Stats& stats() const { return stats_; }
    size_t table_count() const;

private:
    void write_internal(const WriteBatch& batch);
    LookupResult lookup(std::string_view key, uint64_t snapshot) const;

    void open_tables();
    void flush_memtable();
    void maybe_schedule_maintenance();
    void maintenance_loop();
    std::string table_path(uint64_t number) const;
    std::string wal_path() const;

    Config config_;
    mutable Stats stats_;

    // Writers are serialized by write_mutex_. Readers never take it.
    std::mutex write_mutex_;
    uint64_t next_sequence_ = 1;
    std::atomic<uint64_t> visible_sequence_{0};

    // Guards the memtable pointers against a flush swapping them.
    mutable std::shared_mutex memtable_mutex_;
    std::unique_ptr<SkipList> memtable_;
    std::unique_ptr<SkipList> immutable_;

    // Tables ordered oldest to newest.
    mutable std::shared_mutex tables_mutex_;
    std::vector<std::shared_ptr<SSTable>> tables_;
    uint64_t next_table_number_ = 1;

    std::unique_ptr<WAL> wal_;

    // Flushes run one at a time.
    std::mutex maintenance_mutex_;
    std::mutex worker_mutex_;
    std::condition_variable worker_cv_;
    bool worker_stop_ = false;
    bool worker_requested_ = false;
    std::thread worker_;
};

}  // namespace velocitydb
