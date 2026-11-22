// SPDX-License-Identifier: MIT
//
// The shared text helpers (issue #733). These replace 26 copies of `lower`, 24 of `trim` and 8 of
// `split_top_level`, so the properties asserted here are the ones the copies disagreed about:
// locale independence, the CSS whitespace set, and how a malformed value splits.

#include "tessera/detail/text_util.hpp"

#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace detail = tessera::detail;

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::cerr << "text_util: FAIL " << what << "\n";
        ++g_failures;
    }
}

void test_lower() {
    check(detail::lower("ABC") == "abc", "lower maps A-Z");
    check(detail::lower("aBc123") == "abc123", "lower leaves digits and lower case alone");
    check(detail::lower("") == "", "lower of empty is empty");
    // The copies used `std::tolower`, which reads the C locale. The arithmetic version must not.
    check(detail::lower("I") == "i", "lower maps I to i regardless of locale");
    // A UTF-8 continuation byte (0x80-0xBF) must pass through untouched: it is not an ASCII letter,
    // and reinterpreting it would corrupt the string.
    const std::string utf8 = "\xC3\x84"; // "Ã„"
    check(detail::lower(utf8) == utf8, "lower leaves non-ASCII bytes untouched");
}

void test_trim() {
    check(detail::trim("  abc  ") == "abc", "trim removes spaces");
    check(detail::trim("\t\r\n\f abc \f\n\r\t") == "abc", "trim removes every CSS whitespace char");
    check(detail::trim("   ") == "", "trim of all whitespace is empty");
    check(detail::trim("") == "", "trim of empty is empty");
    check(detail::trim("abc") == "abc", "trim of a clean string is itself");
    // Vertical tab is NOT CSS whitespace, so it must survive. The copies that used `std::isspace`
    // removed it, which is one of the dialects this header removes.
    check(detail::trim("\vabc\v") == "\vabc\v", "trim keeps vertical tab (not CSS whitespace)");
}

void test_normalized() {
    check(detail::normalized("  LINEAR-GRADIENT  ") == "linear-gradient",
          "normalized trims and lowers");
}

void test_split_top_level() {
    const auto parts = detail::split_top_level("a, b, c", ',');
    check(parts.size() == 3, "split yields three parts");
    check(parts[0] == "a" && parts[1] == " b" && parts[2] == " c",
          "split does not trim (trimming is a grammar decision)");

    // A separator inside parentheses stays inside its part.
    const auto nested = detail::split_top_level("rgb(1, 2, 3), blue", ',');
    check(nested.size() == 2, "a nested comma does not split");
    check(nested[0] == "rgb(1, 2, 3)" && nested[1] == " blue", "the nested part is intact");

    // An unbalanced ')' must not drive the depth negative and swallow later separators.
    const auto unbalanced = detail::split_top_level("a), b", ',');
    check(unbalanced.size() == 2, "an unbalanced close paren does not collapse the split");

    const auto empty = detail::split_top_level("", ',');
    check(empty.size() == 1 && empty[0].empty(), "split of empty yields one empty part");
}

void test_split_commas() {
    const auto parts = detail::split_commas(" a , b ,c ");
    check(parts.size() == 3, "split_commas yields three parts");
    check(parts[0] == "a" && parts[1] == "b" && parts[2] == "c", "split_commas trims each part");

    // Empty parts are kept: a doubled comma is a grammar error the caller must be able to see.
    const auto trailing = detail::split_commas("a, b,");
    check(trailing.size() == 3 && trailing[2].empty(), "split_commas keeps a trailing empty part");

    const auto nested = detail::split_commas("linear-gradient(red, blue), none");
    check(nested.size() == 2, "split_commas keeps a nested comma inside its part");
}

void test_parse_number() {
    double value = 0.0;
    check(detail::parse_number("12.5px", value) == "px" && value == 12.5,
          "parse_number splits value and unit");
    check(detail::parse_number("-3", value) == "" && value == -3.0,
          "parse_number reads a negative");
    check(detail::parse_number("+4em", value) == "em" && value == 4.0, "parse_number reads a plus");
    check(detail::parse_number("abc", value).empty(), "parse_number rejects a non-number");
    check(detail::parse_number("", value).empty(), "parse_number rejects empty");
    // A lone sign or dot is not a number.
    check(detail::parse_number("-", value).empty(), "parse_number rejects a lone sign");
    check(detail::parse_number(".", value).empty(), "parse_number rejects a lone dot");
}

} // namespace

int main() {
    test_lower();
    test_trim();
    test_normalized();
    test_split_top_level();
    test_split_commas();
    test_parse_number();
    if (g_failures != 0) {
        std::cerr << "text_util: " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "text_util: ok\n";
    return 0;
}
