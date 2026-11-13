// SPDX-License-Identifier: MIT

// ADR-040 generated base: `gen::<Component>Base<Self>` (docs/design/compiler.md section 7). The
// legacy emitters produce a header and a source for a plain struct; this seam derives the
// header-only CRTP template from them so both paths share one emitter set.

#pragma once

#include "codegen_internal.hpp"

#include <optional>
#include <string>
#include <vector>

namespace tessera::compiler::codegen::internal {

// The C++ shape of the member a `bind::` id requires on the developer class.
struct BindingShape {
    std::string return_type; // `void` when the binding returns nothing
    std::string param_type;  // empty when the member takes no argument
    std::string param_name;  // the generated parameter name, empty with no argument
    bool event = false;
};

// Null with `error` set when the binding names a payload or return type with no C++ form.
[[nodiscard]] std::optional<BindingShape> binding_shape(const ir::Binding& binding,
                                                        std::string& error);

// `void sync()` / `void on_click(const tessera::ui::ClickEvent& event)`: the declaration text a
// developer writes (the base's diagnostics print it).
[[nodiscard]] std::string binding_signature(const ir::Binding& binding, const BindingShape& shape);

// Emits the in-class dispatch thunks (one per binding, calling the same-named member of `Self`)
// and the protected-style accessors (`x()` per state, `ref_<name>()` per ref, `<name>()` per prop
// and animated value). Fails on an accessor name that collides with a generated member.
[[nodiscard]] bool emit_base_members(CppWriter& writer, const ir::Component& component,
                                     const std::vector<const ir::Binding*>& bindings,
                                     std::string& error);

// Builds `<Component>.g.hpp` from the legacy header/source texts of the same component.
[[nodiscard]] bool emit_generated_base(const ir::Component& component,
                                       const std::vector<const ir::Binding*>& bindings,
                                       const std::string& legacy_header,
                                       const std::string& legacy_source, GeneratedFile& out,
                                       std::string& error);

} // namespace tessera::compiler::codegen::internal
