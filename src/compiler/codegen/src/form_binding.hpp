// SPDX-License-Identifier: MIT

#pragma once

#include "codegen_internal.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The generated form-state binding of the library `Form` component
// (docs/specs/forms-and-validation-v1.md section 5.2, docs/design/compiler.md section 11).
//
// `Form` and `Field` stay ordinary declared components: their IR is data and their generated
// structs are the same as any other component's. The *binding* lives in the component that owns the
// `Form` instance (the caller), because the `Field` instances are caller-owned slot content and
// component instances are never inlined. For every `Form` instance the caller gets one
// `tessera::ui::forms::Form` controller member; the collector below records the seed list, the
// Field bindings (by `name`) and their controller rules. Codegen emits the seeding, the
// controller -> Field `value`/`error`/`errors` refresh, submit/validate and focus-first-invalid
// paths from this plan; no validation rule is evaluated in generated code, the forms library is.
namespace tessera::compiler::codegen::internal {

// One controller rule distilled from a Field's `required`/`validators` prop data. The forms library
// evaluates it (docs/specs/forms-and-validation-v1.md section 8.1).
struct FormRule {
    enum class Kind { Required, MinLength, MaxLength, Pattern };
    Kind kind = Kind::Required;
    std::int64_t number = 0; // MinLength/MaxLength bound
    std::string pattern;     // Pattern glob
};

struct FormFieldBinding {
    unsigned ordinal = 0;        // Field instance pre-order ordinal (generated child_N_/node_N)
    unsigned form_ordinal = 0;   // enclosing Form instance pre-order ordinal
    std::string name;            // literal `name` prop: the stable form path
    std::string symbol;          // sanitized name used in generated member names
    unsigned widget_ordinal = 0; // first slot child, or 0 when the Field wraps nothing
    bool text_widget = false;    // the slot root is a TextInput/TextArea (edit wiring is generated)
    std::vector<FormRule> rules; // required/validators as data, deduped, declaration order
};

struct FormBinding {
    unsigned ordinal = 0; // Form instance pre-order ordinal
    std::string symbol;   // sanitized component id, e.g. `form_2_`
    std::vector<FormFieldBinding> fields;
};

struct FormPlan {
    std::vector<FormBinding> forms;

    [[nodiscard]] bool empty() const {
        return forms.empty();
    }
    [[nodiscard]] const FormBinding* form(unsigned ordinal) const;
    [[nodiscard]] const FormFieldBinding* field(unsigned ordinal) const;
};

// Collects the form bindings of one component. Rejected loudly with `error` (the caller turns it
// into TESSERA_CODEGEN_UNSUPPORTED): a nested `Form`, a Field inside a Form without a static
// `name`, a duplicate field name, an explicit `value`/`defaultValue`/`error`/`errors` on a
// form-bound Field (the controller owns those props), a non-literal `initial`/`required`/
// `validators`, a malformed `name=value` seed, an unknown validator rule, or a rule count beyond
// the forms model's cap.
[[nodiscard]] bool collect_form_plan(const ir::IrModule& module, const ir::Component& component,
                                     FormPlan& plan, std::string& error);

// True when the component record is the library component declared by the compiler (`Field` or
// `Form`); a project component of the same name wins and is not recognized here.
[[nodiscard]] bool is_library_component(const ir::Component& component, std::string_view name);

// The generated member names of one form binding.
[[nodiscard]] std::string form_member(unsigned ordinal);
[[nodiscard]] std::string form_field_member(unsigned ordinal, std::string_view symbol,
                                            std::string_view suffix);

// The generated edit-callback names of one form-bound text Field: the component method and its
// scene-event adapter.
[[nodiscard]] std::string form_field_handler(unsigned form_ordinal, std::string_view symbol);
[[nodiscard]] std::string form_field_adapter(unsigned form_ordinal, std::string_view symbol);

// A Field name as a member-name suffix: every non-alphanumeric byte becomes `_`.
[[nodiscard]] std::string form_symbol(std::string_view text);

} // namespace tessera::compiler::codegen::internal
