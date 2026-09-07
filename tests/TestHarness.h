// SPDX-License-Identifier: GPL-2.0+

#pragma once

#include <stdexcept>
#include <string>
#include <vector>
#include <cmath>

namespace CubicTest {

struct TestCase {
    const char *name;
    void (*function)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

class Registrar {
public:
    Registrar(const char *name, void (*function)()) {
        registry().push_back({name, function});
    }
};

inline void require(bool condition, const char *expression, const char *file, int line) {
    if (!condition) {
        throw std::runtime_error(std::string(file) + ":" + std::to_string(line) +
                                 ": requirement failed: " + expression);
    }
}

inline void requireNear(double actual, double expected, double tolerance,
                        const char *file, int line) {
    if (std::fabs(actual - expected) > tolerance) {
        throw std::runtime_error(std::string(file) + ":" + std::to_string(line) +
                                 ": expected " + std::to_string(actual) + " to be within " +
                                 std::to_string(tolerance) + " of " + std::to_string(expected));
    }
}

} // namespace CubicTest

#define CUBIC_TEST(name) \
    static void name(); \
    static CubicTest::Registrar name##_registrar(#name, &name); \
    static void name()

#define CUBIC_REQUIRE(expression) \
    CubicTest::require(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

#define CUBIC_REQUIRE_NEAR(actual, expected, tolerance) \
    CubicTest::requireNear((actual), (expected), (tolerance), __FILE__, __LINE__)
