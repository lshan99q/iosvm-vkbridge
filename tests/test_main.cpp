// SPDX-License-Identifier: MIT

#include "test_framework.hpp"

#include <exception>
#include <iostream>

namespace vkbtest
{

std::vector<Case> &registry()
{
    static std::vector<Case> cases;
    return cases;
}

Registrar::Registrar(const char *name, Function function)
{
    registry().push_back(Case{name, function});
}

} // namespace vkbtest

int main()
{
    size_t passed = 0;
    size_t failed = 0;
    for (const auto &test : vkbtest::registry()) {
        try {
            test.function();
            std::cout << "[ok] unit: " << test.name << "\n";
            ++passed;
        } catch (const vkbtest::Failure &failure) {
            std::cout << "[FAIL] unit: " << test.name << ": " << failure.message << "\n";
            ++failed;
        } catch (const std::exception &error) {
            std::cout << "[FAIL] unit: " << test.name << ": unexpected exception: " << error.what()
                      << "\n";
            ++failed;
        }
    }
    std::cout << "unit tests: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
