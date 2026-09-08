//
// Minimal self-contained test framework.
//

#ifndef TEST_RUNNER_H
#define TEST_RUNNER_H

#include <iostream>
#include <functional>
#include <string>
#include <vector>
#include <sstream>

#define ASSERT_TRUE(cond) \
    do { if (!(cond)) { \
        std::cerr << "  FAIL [" << __FILE__ << ":" << __LINE__ << "] expected true: " << #cond << "\n"; \
        return false; \
    }} while (0)

#define ASSERT_FALSE(cond) \
    do { if ((cond)) { \
        std::cerr << "  FAIL [" << __FILE__ << ":" << __LINE__ << "] expected false: " << #cond << "\n"; \
        return false; \
    }} while (0)

#define ASSERT_EQ(a, b) \
    do { \
        auto _a = (a); auto _b = (b); \
        if (_a != _b) { \
            std::cerr << "  FAIL [" << __FILE__ << ":" << __LINE__ << "] " \
                      << #a << " (" << _a << ") != " << #b << " (" << _b << ")\n"; \
            return false; \
        } \
    } while (0)

#define ASSERT_NE(a, b) \
    do { \
        auto _a = (a); auto _b = (b); \
        if (_a == _b) { \
            std::cerr << "  FAIL [" << __FILE__ << ":" << __LINE__ << "] " \
                      << #a << " == " << #b << " (both " << _a << ")\n"; \
            return false; \
        } \
    } while (0)

#define ASSERT_STREQ(a, b) \
    do { \
        std::string _a(a); std::string _b(b); \
        if (_a != _b) { \
            std::cerr << "  FAIL [" << __FILE__ << ":" << __LINE__ << "] " \
                      << "\"" << _a << "\" != \"" << _b << "\"\n"; \
            return false; \
        } \
    } while (0)

struct Test {
    std::string name;
    std::function<bool()> fn;
};

static int run_tests(const char* suite, const std::vector<Test>& tests) {
    std::cout << "=== " << suite << " ===\n";
    int passed = 0, failed = 0;
    for (const auto& t : tests) {
        bool ok = t.fn();
        if (ok) {
            std::cout << "  [PASS] " << t.name << "\n";
            ++passed;
        } else {
            std::cout << "  [FAIL] " << t.name << "\n";
            ++failed;
        }
    }
    std::cout << passed << "/" << (passed + failed) << " passed";
    if (failed) std::cout << "  *** " << failed << " FAILED ***";
    std::cout << "\n\n";
    return failed == 0 ? 0 : 1;
}

#endif // TEST_RUNNER_H