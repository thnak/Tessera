// SPDX-License-Identifier: MIT

// ADR-050 section 3.1 (AF5): the `useService<T>()` lowering of the generated base. This TU owns the
// whole service surface of `<Component>.g.hpp`: the private typed slot tuple,
// `attach_services()`/`mount(context)` which resolve every declared service exactly once, the typed
// `service<T>()` accessor whose `static_assert` is the undeclared-service diagnostic, and one
// `<name>()` accessor per requirement.
//
// Emission is conditional on `component.services` being non-empty, which is what keeps every
// committed `.g.hpp` golden byte-identical (AGENTS.md: a golden moves only when its generator
// output really changed). A component that calls no `useService` therefore generates no service
// code at all, exactly as ADR-050 section 3 item 4 requires.

#pragma once

#include "codegen_internal.hpp"

#include <set>
#include <string>
#include <vector>

namespace tessera::compiler::codegen::internal {

// True when the component declares at least one service requirement.
[[nodiscard]] bool uses_services(const ir::Component& component);

// The project headers that declare the component's service interfaces, unique and sorted, in the
// `#include "..."` spelling (no quotes). Empty without services.
[[nodiscard]] std::vector<std::string> service_headers(const ir::Component& component);

// Emits the service fields of a component: the private typed slot tuple. `apply_access` puts it in
// the struct's private run (its name ends in `_`, like every generated field). Emits nothing for a
// component without services. Fails on two requirements that name the same interface or binding.
[[nodiscard]] bool emit_service_fields(CppWriter& writer, const ir::Component& component,
                                       std::string& error);

// Claims every generated service member name in the component's `names` set (the accessors, the
// predicates and one name per requirement), so a TSX binding that collides with any of them is the
// usual `TESSERA_CODEGEN_UNSUPPORTED` collision instead of a redeclaration error.
[[nodiscard]] bool claim_service_names(const ir::Component& component, std::set<std::string>& names,
                                       std::string& error);

// Emits the public service surface: `attach_services()`, `mount(context)`, the `service<T>()`
// accessor with its `TESSERA_CG_SERVICE_*` diagnostics, and one `<name>()` accessor per
// requirement. Emits nothing for a component without services.
[[nodiscard]] bool emit_service_accessors(CppWriter& writer, const ir::Component& component,
                                          std::string& error);

} // namespace tessera::compiler::codegen::internal
