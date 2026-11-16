// SPDX-License-Identifier: MIT
//
// AF8 (issue #195) entry point. One executable per area, matching the repository's unit-test
// convention; the groups live in the sibling TUs:
//
//   * appfw_i18n_locale_tests.cpp   - normalization and the section 8.1 negotiation chain
//   * appfw_i18n_catalog_tests.cpp  - stable ids, the fallback chain, missing keys, validation
//   * appfw_i18n_service_tests.cpp  - the change signal, the epoch, the per-window override
//   * appfw_i18n_relayout_tests.cpp - the dirty mask a locale change produces, per case
//   * appfw_i18n_layout_tests.cpp   - the measured box through the real Yoga layout engine
//
// Every group is deterministic: explicit locale and message tables, `StubTextMeasurer` with
// `StubFontMetricsProvider`, no wall clock, no OS locale, no font file and no file system.

#include "appfw_i18n_test_support.hpp"

#include <iostream>

namespace i18n_test {

int run_locale_tests();
int run_catalog_tests();
int run_service_tests();
int run_relayout_tests();
int run_layout_tests();

} // namespace i18n_test

int main() {
    const int locale = i18n_test::run_locale_tests();
    const int catalog = i18n_test::run_catalog_tests();
    const int service = i18n_test::run_service_tests();
    const int relayout = i18n_test::run_relayout_tests();
    const int layout = i18n_test::run_layout_tests();
    (void)locale;
    (void)catalog;
    (void)service;
    (void)relayout;
    (void)layout;

    if (i18n_test::g_failures != 0) {
        std::cerr << "appfw_i18n: " << i18n_test::g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "appfw_i18n: ok\n";
    return 0;
}
