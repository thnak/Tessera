// SPDX-License-Identifier: MIT

#pragma once

#include "codegen_internal.hpp"

#include <string>
#include <string_view>

// The optional stroke and fill-rule props of a `Path` (`strokeLinecap`, `strokeLinejoin`,
// `fillRule`): validated against SVG's closed value sets and written into the synthesized document
// after `stroke-width`. An unset prop is left out, so a `Path` that does not use them keeps the
// exact bytes (and asset identity) it had before they existed.
namespace tessera::compiler::codegen::internal {

struct PathExtras {
    // Empty means unset. Views of string literals in this translation unit, never of user text.
    std::string_view linecap;
    std::string_view linejoin;
    std::string_view fill_rule;
};

// false with a TESSERA_CODEGEN_UNSUPPORTED `error` for a non-literal or unknown value.
[[nodiscard]] bool read_path_extras(const ir::Node& node, PathExtras& extras, std::string& error);

// Appends ` stroke-linecap="..." stroke-linejoin="..." fill-rule="..."` for the set members.
void append_path_extras(const PathExtras& extras, std::string& svg);

} // namespace tessera::compiler::codegen::internal
