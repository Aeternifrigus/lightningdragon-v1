#include "velocitydb.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>

#include "skiplist.hpp"
#include "sstable.hpp"
#include "wal.hpp"

namespace velocitydb {

namespace fs = std::filesystem;

VelocityDB::VelocityDB(Config config) : config_(std::move(config)) {
    fs::create_directories(config_.data_dir);
    memtable_ = std::make_unique<SkipList>();

    open_tables();
    uint64_t max_seq = 0;
    for (const auto& t : tables_) max_seq = std::max(max_seq, t->max_sequence());
    next_sequence_ = max_seq + 1;

    if (config_.enable_wal) {
        recover_wal();
        wal_ = std::make_unique<WAL>(wal_path());
    }
    visible_sequence_.store(next_sequence_ - 1, std::memory_order_release);

    if (config_.background_maintenance) {
        worker_ = std::thread(&VelocityDB::maintenance_loop, this);
    }
}

VelocityDB::~VelocityDB() {
    if (worker_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(worker_mutex_);
            worker_stop_ = true;
        }
        worker_cv_.notify_all();
        worker_.join();
    }
    try {
        flush_memtable();
        if (wal_) wal_->sync();
    } catch (const std::exception& e) {
        // the WAL still has everything; it is replayed on the next open
        std::cerr << "velocitydb: flush on close failed: " << e.what() << "\n";
    }
}

std::string VelocityDB::table_path(uint64_t number) const {
    char name[32];
    std::snprintf(name, sizeof(name), "%020llu.sst", static_cast<unsigned long long>(number));
    return (fs::path(config_.data_dir) / name).string();
}

std::string VelocityDB::wal_path() const { return (fs::path(config_.data_dir) / "wal.log").string(); }

void VelocityDB::open_tables() {
    std::vector<std::pair<uint64_t, std::string>> found;
    for (const auto& entry : fs::directory_iterator(config_.data_dir)) {
        const auto& p = entry.path();
        if (p.extension() == ".sst") {
            found.emplace_back(std::stoull(p.stem().string()), p.string());
        }
    }
    std::sort(found.begin(), found.end());
    for (const auto& [number, path] : found) {
        tables_.push_back(SSTable::open(path));
        next_table_number_ = number + 1;
    }
}

// Replays wal.log.old (left behind if we crashed during a flush) and wal.log,
// then writes everything recovered to a table so both logs can be deleted.
void VelocityDB::recover_wal() {
    const std::string current = wal_path();
    const std::string old = current + ".old";

    uint64_t max_seq = next_sequence_ - 1;
    for (const auto& path : {old, current}) {
        WAL::replay(path, [&](uint64_t seq, const WriteOp& op) {
            if (op.deleted) {
                memtable_->remove(op.key, seq);
            } else {
                memtable_->put(op.key, op.value, seq);
            }
            max_seq = std::max(max_seq, seq);
        });
    }
    next_sequence_ = max_seq + 1;

    if (memtable_->key_count() > 0) {
        std::vector<SSTable::Entry> entries;
        for (auto it = memtable_->begin(); it.valid(); it.next()) {
            const auto& v = it.newest();
            entries.push_back({it.key(), v.value, v.sequence, v.deleted});
        }
        tables_.push_back(SSTable::write(table_path(next_table_number_++), entries,
                                         {config_.enable_compression, config_.bloom_bits_per_key}));
        memtable_ = std::make_unique<SkipList>();
    }
    fs::remove(old);
    fs::remove(current);
}

// ---------------------------------------------------------------------------
// writes

void VelocityDB::put(std::string_view key, std::string_view value) {
    WriteBatch batch;
    batch.put(key, value);
    write_internal(batch);
}

void VelocityDB::remove(std::string_view key) {
    WriteBatch batch;
    batch.remove(key);
    write_internal(batch);
}

void VelocityDB::write(const WriteBatch& batch) { write_internal(batch); }

void VelocityDB::write_internal(const WriteBatch& batch) {
    if (batch.empty()) return;
    {
        std::lock_guard<std::mutex> writer(write_mutex_);

        const uint64_t first = next_sequence_;
        next_sequence_ += batch.size();
        if (wal_) wal_->append(first, batch.ops());

        uint64_t seq = first;
        for (const auto& op : batch.ops()) {
            if (op.deleted) {
                memtable_->remove(op.key, seq);
            } else {
                memtable_->put(op.key, op.value, seq);
            }
            seq++;
        }
        // Publishing after the whole batch is in makes it visible all at once.
        visible_sequence_.store(seq - 1, std::memory_order_release);
    }

    for (const auto& op : batch.ops()) {
        (op.deleted ? stats_.deletes : stats_.writes).fetch_add(1, std::memory_order_relaxed);
    }
    maybe_schedule_maintenance();
}

// ---------------------------------------------------------------------------
// reads

std::optional<std::string> VelocityDB::get(std::string_view key) const {
    return get(key, create_snapshot());
}

std::optional<std::string> VelocityDB::get(std::string_view key, const Snapshot& snapshot) const {
    LookupResult r = lookup(key, snapshot.sequence);
    stats_.reads.fetch_add(1, std::memory_order_relaxed);
    if (r.state != LookupResult::State::Found) return std::nullopt;
    return std::move(r.value);
}

Snapshot VelocityDB::create_snapshot() const {
    return Snapshot{visible_sequence_.load(std::memory_order_acquire)};
}

LookupResult VelocityDB::lookup(std::string_view key, uint64_t snapshot) const {
    {
        std::shared_lock<std::shared_mutex> lock(memtable_mutex_);
        for (const SkipList* mem : {memtable_.get(), immutable_.get()}) {
            if (!mem) continue;
            LookupResult r = mem->get(key, snapshot);
            if (r.state != LookupResult::State::NotFound) {
                stats_.memtable_hits.fetch_add(1, std::memory_order_relaxed);
                return r;
            }
        }
    }

    // A flush adds its table before dropping the immutable memtable, so a key
    // is always in one place or the other.
    std::shared_lock<std::shared_mutex> lock(tables_mutex_);
    for (auto it = tables_.rbegin(); it != tables_.rend(); ++it) {
        if (!(*it)->may_contain(key)) {
            stats_.bloom_skips.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        LookupResult r = (*it)->get(key, snapshot);
        if (r.state != LookupResult::State::NotFound) {
            stats_.table_hits.fetch_add(1, std::memory_order_relaxed);
            return r;
        }
    }
    return {};
}

size_t VelocityDB::table_count() const {
    std::shared_lock<std::shared_mutex> lock(tables_mutex_);
    return tables_.size();
}

// ---------------------------------------------------------------------------
// flush

void VelocityDB::flush() {
    flush_memtable();
    if (wal_) wal_->sync();
}

void VelocityDB::flush_memtable() {
    std::lock_guard<std::mutex> maintenance(maintenance_mutex_);

    std::string old_log;
    {
        // Holding the writer lock means no write can land between the swap and
        // the log rotation, so wal.log.old matches the immutable memtable.
        std::lock_guard<std::mutex> writer(write_mutex_);
        if (memtable_->key_count() == 0) return;
        std::unique_lock<std::shared_mutex> lock(memtable_mutex_);
        immutable_ = std::move(memtable_);
        memtable_ = std::make_unique<SkipList>();
        if (wal_) old_log = wal_->rotate();
    }

    std::vector<SSTable::Entry> entries;
    entries.reserve(immutable_->key_count());
    for (auto it = immutable_->begin(); it.valid(); it.next()) {
        const auto& v = it.newest();
        entries.push_back({it.key(), v.value, v.sequence, v.deleted});
    }

    auto table = SSTable::write(table_path(next_table_number_++), entries,
                                {config_.enable_compression, config_.bloom_bits_per_key});
    {
        std::unique_lock<std::shared_mutex> lock(tables_mutex_);
        tables_.push_back(table);
    }
    {
        std::unique_lock<std::shared_mutex> lock(memtable_mutex_);
        immutable_.reset();
    }
    if (!old_log.empty()) fs::remove(old_log);

    stats_.flushes.fetch_add(1, std::memory_order_relaxed);
    stats_.table_bytes_raw.fetch_add(table->raw_bytes(), std::memory_order_relaxed);
    stats_.table_bytes_stored.fetch_add(table->stored_bytes(), std::memory_order_relaxed);
}

void VelocityDB::maybe_schedule_maintenance() {
    if (!worker_.joinable()) return;

    bool needed;
    {
        std::shared_lock<std::shared_mutex> lock(memtable_mutex_);
        needed = !immutable_ && memtable_->memory_usage() >= config_.memtable_size_limit;
    }
    if (!needed) return;
    {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        worker_requested_ = true;
    }
    worker_cv_.notify_one();
}

void VelocityDB::maintenance_loop() {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(worker_mutex_);
            worker_cv_.wait(lock, [this] { return worker_stop_ || worker_requested_; });
            if (worker_stop_) return;
            worker_requested_ = false;
        }
        try {
            bool flush_needed;
            {
                std::shared_lock<std::shared_mutex> lock(memtable_mutex_);
                flush_needed = memtable_->memory_usage() >= config_.memtable_size_limit;
            }
            if (flush_needed) flush_memtable();
        } catch (const std::exception& e) {
            std::cerr << "velocitydb: background maintenance failed: " << e.what() << "\n";
        }
    }
}

}  // namespace velocitydb
