// SPDX-License-Identifier: MIT

#pragma once

#include <iostream>
#include <string_view>

// Shared fixtures for the preview's author-stylesheet cases (issues #390 and #463).
//
// Two translation units share the failure counter so one ctest case reports one total and one
// `main()`: `scene_stylesheet_tests.cpp` covers the sheet that is compiled as written,
// `scene_stylesheet_module_tests.cpp` covers the `*.module.css` that is scanned first. They are
// separate files only to stay inside the source-size budget.
namespace scene_stylesheet_test {

inline int& failures() {
    static int count = 0;
    return count;
}

inline void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "scene_stylesheet: " << message << "\n";
        ++failures();
    }
}

// The `*.module.css` cases of `scene_stylesheet_module_tests.cpp`, so `main()` stays one line.
void run_module_cases();

} // namespace scene_stylesheet_test
