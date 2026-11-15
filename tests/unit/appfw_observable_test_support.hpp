// SPDX-License-Identifier: MIT
//
// Shared harness for the `appfw::Observable<T>` tests (issue #176). Both test files are compiled
// into one `appfw_observable` binary together with the allocation proof, so they share the
// failure counter and the recorder here instead of duplicating them.

#pragma once

#include "tessera/appfw/observable.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace appfw_test {

namespace appfw = tessera::appfw;
namespace ui = tessera::ui;

inline int failures = 0;

// Two overloads on purpose: a literal message allocates nothing, so a measured allocation window
// in `appfw_observable_allocation.cpp` is never polluted by the harness itself. A dynamic message
// (a `std::to_string` result) may allocate, so those checks stay outside a measured window.
inline bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "appfw_observable: " << message << "\n";
        ++failures;
    }
    return condition;
}

inline bool expect(bool condition, const std::string& message) {
    return expect(condition, message.c_str());
}

// One observer that records what it saw, so delivery order and payload are both assertable.
struct Recorder {
    std::vector<int> values;
    int calls = 0;

    static void observe(void* context, const int& value) {
        auto* self = static_cast<Recorder*>(context);
        ++self->calls;
        self->values.push_back(value);
    }
};

// Defined in appfw_observable_allocation.cpp: true when every measured window stayed off the heap.
[[nodiscard]] bool observable_set_allocates_nothing();

// Defined in appfw_observable_subscription_tests.cpp.
[[nodiscard]] bool subscription_semantics();

} // namespace appfw_test
