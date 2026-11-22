// SPDX-License-Identifier: MIT
//
// The assertion vocabulary of the repo's existing test style: a `main()` returning nonzero, a clear
// message on stderr, and a failure count (`docs/design/testing.md` section 12). This is *not* a
// test framework and not a runner: it is the four functions every `tests/unit/*_tests.cpp` already
// re-implements, so an app's test can stop re-implementing them and a `tessera_testing` failure
// reads like an engine test's failure.
//
// Why not adopt a framework instead: section 12 records that the decision to adopt one has not been
// made, and a testing target is the wrong place to make it. Everything here is header-only and
// `inline`, so a test that links `tessera_testing` and a test that links nothing both pay nothing
// for the parts they do not use.
//
// The failure count is per-binary, not per-test-case: `tessera_testing` has no registry, so a
// fixture class with several `test_*` methods (the existing style) shares one counter and one
// `main()`.

#pragma once

#include <concepts>
#include <iostream>
#include <string>
#include <string_view>
#include <type_traits>

namespace tessera::testing {

// The failure count of the test binary. Inline, so every TU of one executable increments the same
// variable without an out-of-line definition the linker would have to deduplicate.
inline int g_failures = 0;

// Records a failure and returns false, so a caller can write `return expect(...)`.
inline bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++g_failures;
    }
    return condition;
}

// How one value is printed in a mismatch message. Most types are streamable and need nothing; an
// enum class and a struct are not, and a comparison that prints `<unprintable>` is still a usable
// failure, which is why this degrades instead of refusing to compile. A test that wants a readable
// name for its own enum adds the operator in its own namespace, and `requires` picks it up.
template<typename T> void print_value(std::ostream& out, const T& value) {
    if constexpr (requires { out << value; }) {
        out << value;
    } else if constexpr (std::is_enum_v<T>) {
        out << "enum(" << static_cast<long long>(value) << ")";
    } else if constexpr (std::same_as<T, bool>) {
        out << (value ? "true" : "false");
    } else {
        out << "<unprintable>";
    }
}

// The same, with the two values printed. `what` names the assertion, so the output says which
// comparison failed rather than leaving the caller to read two bare values.
template<typename Actual, typename Expected>
bool expect_eq(const Actual& actual, const Expected& expected, std::string_view what) {
    if (actual == expected) {
        return true;
    }
    std::cerr << "FAIL: " << what << "\n  expected: ";
    print_value(std::cerr, expected);
    std::cerr << "\n  actual:   ";
    print_value(std::cerr, actual);
    std::cerr << "\n";
    ++g_failures;
    return false;
}

// The exit code a `main()` returns: 0 when nothing failed. A test that ran zero assertions still
// exits 0, exactly as the existing suites do; an unrun test is caught by ctest naming it, not here.
[[nodiscard]] inline int failures() noexcept {
    return g_failures;
}

// One line naming the case that just ran, so a multi-case binary's failure is attributable. Printed
// unconditionally: a green suite's output is not read, and an environment-variable switch here
// would be the one place a test's behaviour depends on the machine it runs on.
inline void note_case(std::string_view name) {
    std::cerr << "case: " << name << "\n";
}

} // namespace tessera::testing
