// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/ir/component.hpp"

#include <span>
#include <string_view>

// Library components of docs/design/component-library.md that the compiler resolves from an
// `import { <Name> } from "tessera"` (docs/specs/jsx-subset-v1.md section 4). A library component
// is an ordinary IR component: it declares props, a node tree and its static `children` slot, so
// lowering emits a `Component` node and codegen instantiates it through the ordinary component
// path. There is no intrinsic node kind for any library component (docs/specs/forms-and-
// validation-v1.md section 5, Q-09).
namespace tessera::compiler::lowering {

// One built-in component: the JSX tag, the IR component id, and the header/base name codegen
// generates it under.
struct LibraryComponent {
    std::string_view name; // JSX tag and generated struct name
    std::string_view id;   // IR component id, `comp::<Name>`
};

// The built-in library components, sorted by name (deterministic).
[[nodiscard]] std::span<const LibraryComponent> library_components();

// The built-in component named `name`, or nullptr when the name is not a library component.
[[nodiscard]] const LibraryComponent* find_library_component(std::string_view name);

// The IR declaration of one library component: its declared prop records (docs/specs/forms-and-
// validation-v1.md section 5.1) and the node tree it builds, including a static `children` slot
// that a Field wrapper places around the caller's widget. Deterministic: the same component always
// yields the same IR, with synthetic source spans under `tessera/components/`.
[[nodiscard]] ir::Component make_library_component(const LibraryComponent& component);

} // namespace tessera::compiler::lowering
