#include "velocitydb.hpp"

namespace velocitydb {

void WriteBatch::put(std::string_view key, std::string_view value) {
    ops_.push_back(WriteOp{std::string(key), std::string(value), false});
}

void WriteBatch::remove(std::string_view key) {
    ops_.push_back(WriteOp{std::string(key), std::string(), true});
}

}  // namespace velocitydb
