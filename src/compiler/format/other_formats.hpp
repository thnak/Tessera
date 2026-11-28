// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/format/format.hpp"

#include <cstddef>
#include <string>
#include <string_view>

// The two non-TSX printers of ADR-061 slice 3. Both take LF-normalised text without a BOM (the
// caller owns line endings and the BOM) and either fill `out` or describe why they refuse.
namespace tessera::compiler::format {

struct Problem {
    std::string code;
    std::string message;
    std::size_t offset = 0;
};

// Token-based CSS printer (docs/specs/source-format-v1.md section 12).
bool format_css(std::string_view source, const FormatOptions& options, std::string& out,
                Problem& problem);

// Key-order-preserving JSON printer for tessera.json (section 13).
bool format_json(std::string_view source, const FormatOptions& options, std::string& out,
                 Problem& problem);

} // namespace tessera::compiler::format
