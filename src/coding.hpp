#pragma once

// Little-endian encoding helpers and CRC32, shared by the WAL and table files.

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace velocitydb {

inline void put_u32(std::string& out, uint32_t v) {
    for (int i = 0; i < 4; i++) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

inline void put_u64(std::string& out, uint64_t v) {
    for (int i = 0; i < 8; i++) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

inline uint32_t get_u32(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(p[i]) << (8 * i);
    return v;
}

inline uint64_t get_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

inline uint32_t crc32(const void* data, size_t size) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t crc = 0xFFFFFFFFu;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

inline uint32_t crc32(std::string_view s) { return crc32(s.data(), s.size()); }

}  // namespace velocitydb
