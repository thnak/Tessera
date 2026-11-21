// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/ast.hpp"

#include <cstddef>
#include <string>

namespace tessera::compiler::rules {

// One `environment` hook call after the form check (docs/specs/jsx-subset-v1.md section 7.1):
// `const bp = useBreakpoint();`, `const theme = useTheme();`, `const [theme, setTheme] =
// useThemeState();`.
//
// The analyzer records what the call wrote; lowering turns it into an IR `environment[]` entry. The
// manifest is the authority on the return type and the provider name, so the analyzer compares
// against it rather than re-deriving the shape.
struct EnvironmentInfo {
    std::size_t statement = 0; // index into the component body
    // The local name the value was bound to (`bp`). Empty for a bare statement, which is a form
    // error: an environment value nobody reads is dead code, and the IR has no id for it.
    std::string name;
    std::string hook;     // the TSX hook name (`useBreakpoint`)
    std::string provider; // the engine provider name (`breakpoint`)
    // The IR type of the value (`string` for every environment hook today).
    std::string type;
    // True for `useThemeState`, whose second binding is a setter the IR carries as `writable`.
    bool writable = false;
    // The setter's local name for a writable hook (`setTheme`), empty otherwise.
    std::string setter;
    // The one argument a parameterized environment hook takes (`useMediaQuery("(min-width:
    // 600px)")`), or null when the hook takes none. The manifest declares the parameter, so the
    // analyzer checks the form against it rather than special-casing the hook by name.
    const jsx::Expr* argument = nullptr;
};

} // namespace tessera::compiler::rules
