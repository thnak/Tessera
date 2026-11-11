// SPDX-License-Identifier: MIT

// Build-time bootstrap of the library component registry. The IR generator must lower
// `tessera/components/*.tsx` before the generated registry exists, so its executable links this
// empty registry instead of `library_components.cpp` plus the generated translation unit.
//
// The library sources import only `Children` and `TextChangeEvent` from `"tessera"`, which the
// schema resolves without a component declaration. If a future source imports a library component,
// that import does not resolve here and the generator run fails with its ordinary diagnostics
// instead of emitting data from an empty declaration.

#include "tessera/compiler/lowering/library_components.hpp"

#include <span>
#include <string>
#include <string_view>

namespace tessera::compiler::lowering {

std::span<const LibraryComponent> library_components() {
    return {};
}

const LibraryComponent* find_library_component(std::string_view) {
    return nullptr;
}

ir::Component make_library_component(const LibraryComponent& component) {
    ir::Component empty;
    empty.id = std::string(component.id);
    empty.name = std::string(component.name);
    return empty;
}

} // namespace tessera::compiler::lowering
