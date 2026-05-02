#include <cstring>

#include "check.hpp"

// Usage: velocitydb_tests [name-filter]
int main(int argc, char* argv[]) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0;
    int failed_tests = 0;
    for (const auto& test : check::registry()) {
        if (filter && !std::strstr(test.name, filter)) continue;
        const int before = check::failures();
        std::cout << test.name << "\n";
        try {
            test.fn();
        } catch (const std::exception& e) {
            check::fail(__FILE__, __LINE__, std::string("exception: ") + e.what());
        }
        run++;
        if (check::failures() != before) failed_tests++;
    }
    std::cout << "\n" << run - failed_tests << "/" << run << " tests passed\n";
    return failed_tests == 0 ? 0 : 1;
}
