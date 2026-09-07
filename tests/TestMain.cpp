// SPDX-License-Identifier: GPL-2.0+

#include "TestHarness.h"

#include <exception>
#include <iostream>

int main() {
    std::size_t failures = 0;

    for (const CubicTest::TestCase& test : CubicTest::registry()) {
        try {
            test.function();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
        } catch (...) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
        }
    }

    std::cout << CubicTest::registry().size() - failures << "/"
              << CubicTest::registry().size() << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
