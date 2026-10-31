// SPDX-License-Identifier: MIT
#pragma once

#include <sstream>
#include <string>
#include <vector>

namespace vkbtest
{
struct Failure {
    std::string message;
};

using Function = void (*)();

struct Case {
    const char *name;
    Function function;
};

std::vector<Case> &registry();

struct Registrar {
    Registrar(const char *name, Function function);
};

[[noreturn]] inline void fail(const char *file, int line, const std::string &message)
{
    std::ostringstream stream;
    stream << file << ":" << line << ": " << message;
    throw Failure{stream.str()};
}

inline void check(bool condition, const char *expression, const char *file, int line)
{
    if (!condition) {
        fail(file, line, std::string("check failed: ") + expression);
    }
}

template <typename A, typename B>
void check_equal(const A &actual, const B &expected, const char *actual_expression,
                 const char *expected_expression, const char *file, int line)
{
    if (!(actual == expected)) {
        std::ostringstream stream;
        stream << "check failed: " << actual_expression << " == " << expected_expression << " (got "
               << actual << ", expected " << expected << ")";
        fail(file, line, stream.str());
    }
}

} // namespace vkbtest

#define VKB_TEST(name)                                                                             \
    static void name();                                                                            \
    static ::vkbtest::Registrar name##_registrar(#name, &name);                                    \
    static void name()

#define VKB_CHECK(expression) ::vkbtest::check((expression), #expression, __FILE__, __LINE__)

#define VKB_CHECK_EQ(actual, expected)                                                             \
    ::vkbtest::check_equal((actual), (expected), #actual, #expected, __FILE__, __LINE__)
