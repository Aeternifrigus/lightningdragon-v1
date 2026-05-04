#include <random>
#include <string>
#include <vector>

#include "check.hpp"
#include "compressor.hpp"

using velocitydb::Compressor;

namespace {

bool round_trips(const std::vector<uint8_t>& input) {
    const auto packed = Compressor::compress(input.data(), input.size());
    const auto unpacked = Compressor::decompress(packed.data(), packed.size());
    return unpacked && *unpacked == input;
}

std::vector<uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }

}  // namespace

TEST(compressor_round_trips) {
    CHECK(round_trips({}));
    CHECK(round_trips(bytes("a")));
    CHECK(round_trips(bytes("hello hello hello hello hello")));
    CHECK(round_trips(std::vector<uint8_t>(10000, 'x')));

    std::string json;
    for (int i = 0; i < 200; i++) json += R"({"id":)" + std::to_string(i) + R"(,"status":"ok"})";
    CHECK(round_trips(bytes(json)));
}

TEST(compressor_handles_marker_bytes) {
    // 0xFD-0xFF are the format's markers and have to be escaped as literals
    std::vector<uint8_t> input;
    for (int i = 0; i < 300; i++) input.push_back(static_cast<uint8_t>(0xFD + i % 3));
    input.insert(input.end(), 50, 0xFF);
    CHECK(round_trips(input));
}

TEST(compressor_random_data) {
    std::mt19937 gen(42);
    for (size_t size : {1u, 3u, 4u, 100u, 4096u, 70000u}) {
        std::vector<uint8_t> input(size);
        for (auto& b : input) b = static_cast<uint8_t>(gen());
        CHECK(round_trips(input));
    }
}

TEST(compressor_repetitive_data_shrinks) {
    std::string event = R"({"type":"event","action":"page_view","page":"/products"})";
    std::string data;
    for (int i = 0; i < 100; i++) data += event;
    const auto packed = Compressor::compress(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    CHECK(packed.size() * 10 < data.size());
}

TEST(compressor_rejects_bad_input) {
    const std::string data = "abcdabcdabcdabcdabcdabcd";
    auto packed = Compressor::compress(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    CHECK(!Compressor::decompress(packed.data(), 2));
    CHECK(!Compressor::decompress(packed.data(), packed.size() - 1));

    // back-reference pointing before the start of the output
    std::vector<uint8_t> bogus = {0, 0, 0, 8, 0xFF, 8, 0, 20};
    CHECK(!Compressor::decompress(bogus.data(), bogus.size()));
}
