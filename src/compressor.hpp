#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace velocitydb {

// Small LZ77-style compressor for table data: back-references for repeated
// sequences, run-length encoding for repeated bytes, literals otherwise.
//
// Format: 4-byte big-endian original size, then a stream of
//   0xFF len off_hi off_lo   copy `len` bytes from `off` bytes back
//   0xFE len byte            `len` copies of `byte`
//   0xFD byte                literal byte that is >= 0xFD
//   byte                     literal byte < 0xFD
class Compressor {
public:
    static std::vector<uint8_t> compress(const uint8_t* data, size_t size);
    // Returns nullopt if the input is malformed or truncated.
    static std::optional<std::vector<uint8_t>> decompress(const uint8_t* data, size_t size);
};

}  // namespace velocitydb
