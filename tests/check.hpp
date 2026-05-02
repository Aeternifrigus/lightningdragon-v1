#pragma once

// Minimal test harness: TEST(name) registers a function, CHECK and
// CHECK_EQ record failures without stopping the test.

#include <filesystem>
#include <functional>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace check {

struct Test {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Test>& registry() {
    static std::vector<Test> tests;
    return tests;
}

inline int& failures() {
    static int count = 0;
    return count;
}

struct Register {
    Register(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

inline void fail(const char* file, int line, const std::string& message) {
    failures()++;
    std::cerr << "  " << file << ":" << line << ": " << message << "\n";
}

template <typename A, typename B>
void check_eq(const A& a, const B& b, const char* a_expr, const char* b_expr, const char* file, int line) {
    if (!(a == b)) {
        std::ostringstream msg;
        msg << a_expr << " == " << b_expr << " failed";
        if constexpr (std::is_arithmetic_v<A> && std::is_arithmetic_v<B>) msg << " (" << a << " vs " << b << ")";
        fail(file, line, msg.str());
    }
}

// Fresh directory under the system temp dir, removed when it goes out of scope.
class TempDir {
public:
    TempDir() {
        std::random_device rd;
        path_ = std::filesystem::temp_directory_path() / ("velocitydb_test_" + std::to_string(rd()));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    std::string str() const { return path_.string(); }
    std::filesystem::path path() const { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace check

#define CHECK_CONCAT_(a, b) a##b
#define CHECK_CONCAT(a, b) CHECK_CONCAT_(a, b)

#define TEST(name)                                                                \
    static void name();                                                           \
    static check::Register CHECK_CONCAT(register_, name)(#name, name);            \
    static void name()

#define CHECK(cond) \
    do { \
        if (!(cond)) check::fail(__FILE__, __LINE__, "CHECK(" #cond ") failed"); \
    } while (0)

#define CHECK_EQ(a, b) check::check_eq((a), (b), #a, #b, __FILE__, __LINE__)
