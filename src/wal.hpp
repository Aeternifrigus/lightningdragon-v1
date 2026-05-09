#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "velocitydb.hpp"

namespace velocitydb {

// Write-ahead log. Each record holds one write batch:
//
//   u32 crc32(payload) | u32 payload length | payload
//   payload = u64 first sequence | u32 op count | ops
//   op      = u8 deleted | u32 key length | u32 value length | key | value
//
// Ops in a batch get consecutive sequence numbers. append() hands each record
// to the OS straight away; sync() is what makes it survive a power cut.
class WAL {
public:
    explicit WAL(std::string path);
    ~WAL();
    WAL(const WAL&) = delete;
    WAL& operator=(const WAL&) = delete;

    void append(uint64_t first_sequence, const std::vector<WriteOp>& ops);
    void sync();

    // Moves the current log to <path>.old and starts an empty one. Returns the
    // old path so the caller can delete it once its contents are in a table.
    std::string rotate();

    const std::string& path() const { return path_; }

    using ReplayFn = std::function<void(uint64_t sequence, const WriteOp& op)>;
    // Replays every complete record. Stops at the first truncated or corrupt
    // one, which is what a crash in the middle of append() leaves behind.
    // Returns the number of batches replayed.
    static size_t replay(const std::string& path, const ReplayFn& fn);

private:
    void open();

    std::string path_;
    std::FILE* file_ = nullptr;
    std::mutex mutex_;
};

}  // namespace velocitydb
