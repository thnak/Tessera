// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string>

namespace tessera::compiler::ir {

// docs/specs/ir-v1.md section 8. Plain data: the IR never includes parser headers.
struct SourceSpan {
    std::string file; // project-relative, POSIX separators
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
    std::uint32_t line = 1;
    std::uint32_t column = 1;
    std::uint32_t end_line = 1;
    std::uint32_t end_column = 1;
};

} // namespace tessera::compiler::ir
