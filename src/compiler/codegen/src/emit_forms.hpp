// SPDX-License-Identifier: MIT

#pragma once

#include "codegen_internal.hpp"
#include "form_binding.hpp"

#include <string>

// Emission of the generated form-state binding of the library `Form` component
// (docs/specs/forms-and-validation-v1.md section 5.2, docs/design/compiler.md section 11). The
// caller component owns the controller; this emitter writes its header members/methods, the
// seeding plus Field `value`/`error`/`errors` refresh, submit/validate with focus-first-invalid,
// and the edit-event adapters that route a text edit into the controller. No rule is evaluated
// here: `required`/`validators` become `SyncRule` data the forms library evaluates.
namespace tessera::compiler::codegen::internal {

// The extra public headers a component with form bindings needs.
void emit_form_includes(CppWriter& writer, const FormPlan& plan);

// The controller/field members and the public binding methods on the component struct.
void emit_form_declarations(CppWriter& writer, const FormPlan& plan);

// `seed_form_N(); apply_form_fields_N();` immediately after `apply_props()` in build(), before any
// slot content is built, so a Field builds against the seeded controller.
void emit_form_build_calls(CppWriter& writer, const FormPlan& plan);

// `apply_form_fields_N();` after `apply_props()` in apply_state().
void emit_form_state_calls(CppWriter& writer, const FormPlan& plan);

// The anonymous-namespace edit adapters (one per text Field) plus the focus helper.
void emit_form_adapters(CppWriter& writer, const ir::Component& component, const FormPlan& plan);

// The binding method bodies: seeding, field refresh, validate, submit, focus setter and the edit
// handlers.
[[nodiscard]] bool emit_form_source(CppWriter& writer, const ir::Component& component,
                                    const FormPlan& plan, std::string& error);

} // namespace tessera::compiler::codegen::internal
