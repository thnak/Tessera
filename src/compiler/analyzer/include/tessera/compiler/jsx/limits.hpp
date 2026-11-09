// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>

namespace tessera::compiler::jsx {

// Parser limits from docs/specs/jsx-subset-v1.md section 15. Exceeding one is a diagnostic with
// a TESSERA_JSX_LIMIT_* code, never a crash or a hang. The defaults are starting points.
struct Limits {
    std::size_t max_file_bytes = 1024 * 1024;
    std::size_t max_tokens = 200000;
    // Applies to parser recursion and to the height of every AST node, so a long operator chain
    // counts the same as nested parentheses and deleting the AST cannot overflow the stack.
    std::size_t max_nesting_depth = 128;
    std::size_t max_diagnostics = 100;
    std::size_t max_components = 64;
};

} // namespace tessera::compiler::jsx
