#include "wal.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>

#include <unistd.h>

#include "coding.hpp"

namespace velocitydb {

namespace fs = std::filesystem;

WAL::WAL(std::string path) : path_(std::move(path)) {
    const fs::path parent = fs::path(path_).parent_path();
    if (!parent.empty()) fs::create_directories(parent);
    open();
}

WAL::~WAL() {
    if (file_) std::fclose(file_);
}

void WAL::open() {
    file_ = std::fopen(path_.c_str(), "ab");
    if (!file_) throw std::system_error(errno, std::generic_category(), "open " + path_);
}

void WAL::append(uint64_t first_sequence, const std::vector<WriteOp>& ops) {
    std::string payload;
    put_u64(payload, first_sequence);
    put_u32(payload, static_cast<uint32_t>(ops.size()));
    for (const auto& op : ops) {
        payload.push_back(op.deleted ? 1 : 0);
        put_u32(payload, static_cast<uint32_t>(op.key.size()));
        put_u32(payload, static_cast<uint32_t>(op.value.size()));
        payload += op.key;
        payload += op.value;
    }

    std::string record;
    put_u32(record, crc32(payload));
    put_u32(record, static_cast<uint32_t>(payload.size()));
    record += payload;

    std::lock_guard<std::mutex> lock(mutex_);
    if (std::fwrite(record.data(), 1, record.size(), file_) != record.size() ||
        std::fflush(file_) != 0) {
        throw std::system_error(errno, std::generic_category(), "write " + path_);
    }
}

void WAL::sync() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::fflush(file_);
    ::fsync(fileno(file_));
}

std::string WAL::rotate() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::fclose(file_);
    file_ = nullptr;
    const std::string old_path = path_ + ".old";
    fs::rename(path_, old_path);
    open();
    return old_path;
}

size_t WAL::replay(const std::string& path, const ReplayFn& fn) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return 0;
    const std::string buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto* data = reinterpret_cast<const uint8_t*>(buf.data());
    const size_t size = buf.size();

    size_t pos = 0;
    size_t batches = 0;
    while (pos + 8 <= size) {
        const uint32_t crc = get_u32(data + pos);
        const uint32_t len = get_u32(data + pos + 4);
        if (len < 12 || pos + 8 + len > size) break;
        const uint8_t* p = data + pos + 8;
        if (crc32(p, len) != crc) break;

        const uint8_t* end = p + len;
        uint64_t seq = get_u64(p);
        const uint32_t count = get_u32(p + 8);
        p += 12;

        // decode the whole batch before applying any of it
        std::vector<WriteOp> ops;
        ops.reserve(count);
        bool ok = true;
        for (uint32_t i = 0; i < count; i++) {
            if (end - p < 9) {
                ok = false;
                break;
            }
            WriteOp op;
            op.deleted = p[0] != 0;
            const uint32_t klen = get_u32(p + 1);
            const uint32_t vlen = get_u32(p + 5);
            p += 9;
            if (static_cast<size_t>(end - p) < static_cast<size_t>(klen) + vlen) {
                ok = false;
                break;
            }
            op.key.assign(reinterpret_cast<const char*>(p), klen);
            op.value.assign(reinterpret_cast<const char*>(p + klen), vlen);
            p += klen + vlen;
            ops.push_back(std::move(op));
        }
        if (!ok) break;

        for (const auto& op : ops) fn(seq++, op);
        batches++;
        pos += 8 + len;
    }
    return batches;
}

}  // namespace velocitydb
