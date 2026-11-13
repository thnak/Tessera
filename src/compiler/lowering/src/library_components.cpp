// SPDX-License-Identifier: MIT

// Built-in library components (docs/design/component-library.md, docs/design/compiler.md section
// 11) that a module may import from `"tessera"`. `Field` and its form-level container `Form` come
// from docs/specs/forms-and-validation-v1.md section 5: library components with typed prop records,
// a static `children` slot, and no validation logic of their own.
//
// The declarations are no longer hand-built here (issue #70). `packages/tessera-ui/Field.tsx` and
// `packages/tessera-ui/Form.tsx` are the source of truth: the build runs `tessera_codegen
// --emit-library-cpp` over them and compiles the generated translation unit into this library. This
// file is the stable public contract only: the search over the generated registry and the IR
// builder delegate. A generated declaration that does not lower cleanly fails the build; there is
// no hand-built fallback.
//
// The declared prop records remain normative in docs/specs/forms-and-validation-v1.md sections 5.1
// and 5.2; the TSX sources and the spec are kept in sync by
// tests/unit/library_tsx_parity_tests.cpp.

#include "library_components_generated.hpp"
#include "tessera/compiler/lowering/library_components.hpp"

#include <span>
#include <string_view>

namespace tessera::compiler::lowering {

std::span<const LibraryComponent> library_components() {
    return generated::library_components();
}

const LibraryComponent* find_library_component(std::string_view name) {
    for (const LibraryComponent& component : generated::library_components()) {
        if (component.name == name) {
            return &component;
        }
    }
    return nullptr;
}

ir::Component make_library_component(const LibraryComponent& component) {
    return generated::make_library_component(component);
}

} // namespace tessera::compiler::lowering
