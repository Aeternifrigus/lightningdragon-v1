#include "velocitydb.hpp"

#include "skiplist.hpp"

namespace velocitydb {

void WriteBatch::put(std::string_view key, std::string_view value) {
    ops_.push_back(WriteOp{std::string(key), std::string(value), false});
}

void WriteBatch::remove(std::string_view key) {
    ops_.push_back(WriteOp{std::string(key), std::string(), true});
}

Transaction::Transaction(VelocityDB* db, Snapshot snapshot) : db_(db), snapshot_(snapshot) {}

void Transaction::put(std::string_view key, std::string_view value) { batch_.put(key, value); }

void Transaction::remove(std::string_view key) { batch_.remove(key); }

std::optional<std::string> Transaction::get(std::string_view key) const {
    const auto& ops = batch_.ops();
    for (auto it = ops.rbegin(); it != ops.rend(); ++it) {
        if (it->key == key) {
            if (it->deleted) return std::nullopt;
            return it->value;
        }
    }
    return db_->get(key, snapshot_);
}

bool Transaction::commit() {
    if (done_) return false;
    done_ = true;
    return db_->write_internal(batch_, &snapshot_);
}

void Transaction::rollback() {
    done_ = true;
    batch_.clear();
}

}  // namespace velocitydb
