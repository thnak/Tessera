// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/ast.hpp"

#include <string>

namespace tessera::compiler::rules {

// What an arrow function is for (docs/specs/jsx-subset-v1.md section 9). Lowering reads the role
// instead of re-deriving it from the surrounding syntax.
enum class ArrowRole {
    NativeHandler, // `// Note: bind::id` + `const f = (...) => ...` (spec 9.3)
    InlineHandler, // un-annotated `const f = arrow` made of setter calls only (decision 4)
    Effect,        // first argument of `useEffect`
    Cleanup,       // `return () => ...;` as the last statement of an effect body
    EventArrow,    // an arrow written directly as a JSX attribute value
    ListCallback,  // the callback of `.map`
    Other,         // any other arrow; rejected or typed by later passes
};

struct ArrowInfo {
    const jsx::Expr* arrow = nullptr;
    ArrowRole role = ArrowRole::Other;
    std::string bind_id; // NativeHandler, and the Effect that owns it; empty otherwise
};

} // namespace tessera::compiler::rules
