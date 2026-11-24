// SPDX-License-Identifier: MIT
//
// The class table itself (ADR-058 sections 2 and 4, issue #546).
//
// Every row is a fact the ADRs already decided; nothing here is a new policy. The table is the
// checker's only opinion - `port-check` reads `package.json`, looks each dependency up here, and
// reports the class and the replacement. That is what makes "where does this port stand" a question
// with an answer rather than a reading exercise.

#include "class_table.hpp"

namespace tessera::port_check {
namespace {

// `react` is the one import that is not a dependency problem: the front end accepts it as an alias
// of `"tessera"` (subset 4, `react-compat.md` 4.1), so a port keeps its import lines.
constexpr ClassRow kRows[] = {
    {"react",
     PortClass::SourceCompatible,
     {},
     "react-compat.md 4.1: `react` is an alias of `tessera`; the hooks subset is accepted"},
    {"react-dom", PortClass::BuildTime, "nothing: the renderer is the engine's",
     "ADR-058 section 2: there is no DOM to render into; delete the entry point it imports"},

    // Class B: a Tessera module provides the API under the same import (ADR-058 section 2).
    {"react-router",
     PortClass::Shim,
     {},
     "ADR-058 section 5 item 3: the router shim lowers to the existing IR surface"},
    {"react-router-dom", PortClass::Shim, {}, "ADR-058 section 5 item 3: the router shim"},

    // Class C: rewritten by the build. `*.module.css` is already scoped by the CLI (issue #463).
    {"vite", PortClass::BuildTime, "tessera.json plus the codegen CLI",
     "ADR-058 section 4: the build is the codegen CLI, and node_modules is never read"},

    // Class D: no equivalent. Each row names what to write instead - ADR-058 section 1 forbids a
    // silent drop, and a port author who reads "D" with no alternative learns nothing.
    {"@mui/material", PortClass::NotPorted, "the `tessera-ui` library components",
     "ADR-058 section 1: component libraries map to `tessera-ui`"},
    {"@chakra-ui/react", PortClass::NotPorted, "the `tessera-ui` library components",
     "ADR-058 section 1: component libraries map to `tessera-ui`"},
    {"antd", PortClass::NotPorted, "the `tessera-ui` library components",
     "ADR-058 section 1: component libraries map to `tessera-ui`"},
    {"@emotion/react", PortClass::NotPorted, "a stylesheet plus `className`",
     "ADR-058 section 1: CSS-in-JS has no compile-time key set"},
    {"styled-components", PortClass::NotPorted, "a stylesheet plus `className`",
     "ADR-058 section 1: CSS-in-JS has no compile-time key set"},
    {"i18next", PortClass::NotPorted, "the i18n shim (ADR-058 section 5 item 3)",
     "ADR-058 section 5 item 3: i18n is a shim over the Environment surface"},
    {"@tanstack/react-query", PortClass::NotPorted,
     "the server-state shim (ADR-058 section 5 item 5)",
     "ADR-058 section 5 item 5: server state is a later step"},
    {"framer-motion", PortClass::NotPorted, "`animation` in the stylesheet",
     "ADR-058 section 5 item 5: motion is a later step; the stylesheet animates today"},
};

// The fallback for a dependency no row names. It is `NotPorted` rather than an optimistic guess:
// ADR-058 section 4 says a package that is neither A, B nor C is a class-D diagnostic, so silence
// would be the one answer the ADR rules out.
constexpr ClassRow kWildcard{
    "*", PortClass::NotPorted,
    "check whether the API exists in `tessera-ui` or write it in the developer class",
    "ADR-058 section 4: a package that is neither A, B nor C is class D"};

} // namespace

char class_letter(PortClass value) noexcept {
    switch (value) {
    case PortClass::SourceCompatible:
        return 'A';
    case PortClass::Shim:
        return 'B';
    case PortClass::BuildTime:
        return 'C';
    case PortClass::NotPorted:
        return 'D';
    }
    return '?'; // unreachable: the switch has no default, so a new class is a compile error
}

std::span<const ClassRow> class_rows() noexcept {
    return {kRows, sizeof(kRows) / sizeof(kRows[0])};
}

const ClassRow* find_row(std::string_view package) noexcept {
    for (const ClassRow& row : kRows) {
        if (row.package == package) {
            return &row;
        }
    }
    return &kWildcard;
}

} // namespace tessera::port_check
