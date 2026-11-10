# SPDX-License-Identifier: MIT

# ICU locale-pack probes (task-101). Registered from `tests/unit/CMakeLists.txt`, guarded by
# `TESSERA_ICU_REDUCED_DATA` (CI keeps the option off, so these tests are absent by default).

add_executable(icu_locale_pack_tests
    icu_locale_pack_tests.cpp)

# The runtime header is internal to the text module, and the ICU headers come through the
# tessera_icu interface target (never through tessera_text, which keeps them private).
target_link_libraries(icu_locale_pack_tests PRIVATE tessera_text tessera_icu)

target_include_directories(icu_locale_pack_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/ui/text)

add_test(NAME icu_locale_pack COMMAND icu_locale_pack_tests)

add_executable(text_breaks_parity_tests
    text_breaks_parity_tests.cpp)

target_link_libraries(text_breaks_parity_tests PRIVATE tessera_text)

target_include_directories(text_breaks_parity_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/ui/text)

add_test(NAME text_breaks_parity COMMAND text_breaks_parity_tests)

add_executable(locale_resolver_tests
    locale_resolver_tests.cpp)

target_link_libraries(locale_resolver_tests PRIVATE tessera_text)

target_include_directories(locale_resolver_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/ui/text)

add_test(NAME locale_resolver COMMAND locale_resolver_tests)
