// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/detail/cache_line.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::style {

struct Declaration {
    std::string property;
    std::string value;
    bool important = false;
    std::size_t offset = 0;
};

// `alignas(64)` (issue #732): `std::string` (32) + `std::vector` (24) + `std::size_t` (8) is
// exactly 64, so with the natural 8-byte alignment seven of eight rules straddle a line and a walk
// over `std::vector<Rule>` touches both. The alignment changes no offset.
//
// MSVC's C4324 ("structure was padded due to alignment specifier") fires on this, and on this
// configuration only it is an error rather than a warning, because `TESSERA_WARNINGS_AS_ERRORS` is
// on. The warning is *correct* and describes the intent, so the suppression is the narrow one
// `detail/cache_line.hpp` already establishes for exactly this case: around the declaration, with
// the reason stated here, rather than on the target or in the flags.
TESSERA_CACHE_LINE_BEGIN
struct alignas(64) Rule {
    std::string selector;
    std::vector<Declaration> declarations;
    std::size_t offset = 0;
};
TESSERA_CACHE_LINE_END

struct AtRule {
    std::string name;
    std::string prelude;
    std::string block;
    bool has_block = false;
    std::size_t offset = 0;
};

struct StyleSheet {
    std::vector<Rule> rules;
    std::vector<AtRule> at_rules;
};

StyleSheet parse_stylesheet(std::string_view source);

} // namespace tessera::ui::style
