// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/lowering/library_components.hpp"

#include <span>

// Build-generated registry seam (issue #70). `library_components.cpp` is the public contract;
// `library_components_generated.cpp` is written from `packages/tessera-ui/*.tsx` by
// `tessera_codegen --emit-library-cpp` at build time. The generator links the lowering pipeline but
// not this data, so the registry can be regenerated without a bootstrap cycle.
namespace tessera::compiler::lowering::generated {

// The built-in components, sorted by name: exactly the lowered components of the library TSX
// sources, with their stable declarative ids.
[[nodiscard]] std::span<const LibraryComponent> library_components();

// The IR declaration of `component`, rebuilt from the lowered source. Returns an empty component
// for a name that is not in `library_components()`.
[[nodiscard]] ir::Component make_library_component(const LibraryComponent& component);

} // namespace tessera::compiler::lowering::generated
