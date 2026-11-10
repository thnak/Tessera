// SPDX-License-Identifier: MIT

// Collection and validation of the generated form-state binding (form_binding.hpp,
// docs/specs/forms-and-validation-v1.md section 5.2). The walk mirrors the composition collector's
// pre-order numbering, so a binding ordinal is the same `node_N`/`child_N_` ordinal the emitter
// uses. Everything that cannot be bound statically is rejected here instead of producing generated
// code that silently reads nothing.

#include "emit_props.hpp"
#include "form_binding.hpp"

#include <charconv>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace tessera::compiler::codegen::internal {
namespace {

constexpr std::string_view kFieldFile = "tessera/components/Field.tsx";
constexpr std::string_view kFormFile = "tessera/components/Form.tsx";

// The forms model caps rules per field (FormConfig::max_rules_per_field); the binding must not emit
// a configuration the model would silently truncate.
constexpr std::size_t kMaxGeneratedRules = 8;

bool fail(std::string& error, std::string message) {
    error = "TESSERA_CODEGEN_UNSUPPORTED: " + std::move(message);
    return false;
}

const ir::Expr* find_prop(const ir::Node& node, std::string_view name) {
    const auto found = node.props.find(std::string(name));
    return found == node.props.end() ? nullptr : found->second.get();
}

std::optional<std::string_view> string_literal(const ir::Expr* expr) {
    if (expr == nullptr || expr->kind != ir::ExprKind::Literal) {
        return std::nullopt;
    }
    if (const auto* text = std::get_if<std::string>(&expr->literal)) {
        return std::string_view(*text);
    }
    return std::nullopt;
}

std::optional<std::int64_t> non_negative_int(std::string_view text) {
    std::int64_t value = 0;
    const char* begin = text.data();
    const auto parsed = std::from_chars(begin, begin + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != begin + text.size() || value < 0) {
        return std::nullopt;
    }
    return value;
}

bool add_rule(FormFieldBinding& field, FormRule rule) {
    if (rule.kind == FormRule::Kind::Required) {
        for (const FormRule& existing : field.rules) {
            if (existing.kind == FormRule::Kind::Required) {
                return true;
            }
        }
    }
    field.rules.push_back(std::move(rule));
    return field.rules.size() <= kMaxGeneratedRules;
}

// One `validators` entry as data: `required`, `minLength:N`, `maxLength:N` or `pattern:GLOB`. The
// library evaluates the rule; codegen only encodes it.
bool parse_validator(std::string_view text, FormRule& rule, std::string& why) {
    if (text == "required") {
        rule.kind = FormRule::Kind::Required;
        return true;
    }
    // -1: prefix does not apply; 0: malformed bound; 1: applied. A malformed bound keeps its own
    // diagnostic instead of falling through to the generic "unknown rule" message.
    const auto with_number = [&](std::string_view prefix, FormRule::Kind kind) {
        if (text.rfind(prefix, 0) != 0) {
            return -1;
        }
        const std::optional<std::int64_t> bound = non_negative_int(text.substr(prefix.size()));
        if (!bound) {
            why = "`" + std::string(text) + "` needs a non-negative integer bound";
            return 0;
        }
        rule.kind = kind;
        rule.number = *bound;
        return 1;
    };
    const int minimum = with_number("minLength:", FormRule::Kind::MinLength);
    if (minimum >= 0) {
        return minimum == 1;
    }
    const int maximum = with_number("maxLength:", FormRule::Kind::MaxLength);
    if (maximum >= 0) {
        return maximum == 1;
    }
    if (text.rfind("pattern:", 0) == 0) {
        if (text.size() == 8) {
            why = "`pattern:` needs a glob after the colon";
            return false;
        }
        rule.kind = FormRule::Kind::Pattern;
        rule.pattern = std::string(text.substr(8));
        return true;
    }
    why = "`" + std::string(text) +
          "` is not a supported rule (required, minLength:N, maxLength:N, pattern:GLOB)";
    return false;
}

bool parse_field_rules(const ir::Node& node, FormFieldBinding& field, std::string& error) {
    if (const ir::Expr* required = find_prop(node, "required")) {
        if (required->kind != ir::ExprKind::Literal) {
            return fail(error, "`required` of the form-bound Field `" + field.name +
                                   "` must be a literal bool (a dynamic rule is deferred)");
        }
        const auto* flag = std::get_if<bool>(&required->literal);
        if (flag == nullptr) {
            return fail(error, "`required` of the form-bound Field `" + field.name +
                                   "` must be a literal bool");
        }
        if (*flag && !add_rule(field, FormRule{FormRule::Kind::Required, 0, {}})) {
            return fail(error, "the form-bound Field `" + field.name + "` has more than " +
                                   std::to_string(kMaxGeneratedRules) + " rules");
        }
    }
    const ir::Expr* validators = find_prop(node, "validators");
    if (validators == nullptr) {
        return true;
    }
    if (validators->kind != ir::ExprKind::Array) {
        return fail(error, "`validators` of the form-bound Field `" + field.name +
                               "` must be a literal string array (a dynamic rule is deferred)");
    }
    for (const ir::ExprPtr& item : validators->operands) {
        const std::optional<std::string_view> text = string_literal(item.get());
        if (!text) {
            return fail(error, "`validators` of the form-bound Field `" + field.name +
                                   "` must contain string literals only");
        }
        FormRule rule;
        std::string why;
        if (!parse_validator(*text, rule, why)) {
            return fail(error, "the form-bound Field `" + field.name + "`: " + why);
        }
        if (!add_rule(field, std::move(rule))) {
            return fail(error, "the form-bound Field `" + field.name + "` has more than " +
                                   std::to_string(kMaxGeneratedRules) + " rules");
        }
    }
    return true;
}

bool is_text_widget(const ir::Node& node) {
    return node.kind == ir::NodeKind::TextInput || node.kind == ir::NodeKind::TextArea;
}

bool parse_field(const ir::Node& node, unsigned ordinal, FormFieldBinding& field,
                 std::string& error) {
    field.ordinal = ordinal;
    const std::optional<std::string_view> name = string_literal(find_prop(node, "name"));
    if (!name || name->empty()) {
        return fail(error, "a Field inside a Form needs a string-literal `name`; the controller "
                           "resolves the field by that path");
    }
    field.name = std::string(*name);
    field.symbol = form_symbol(field.name);
    field.widget_ordinal = node.children.empty() ? 0 : ordinal + 1;
    field.text_widget = !node.children.empty() && is_text_widget(node.children.front());
    if (!node.children.empty()) {
        const ir::Node& widget = node.children.front();
        for (const std::string_view owned : {"value", "defaultValue"}) {
            if (find_prop(widget, owned) != nullptr) {
                return fail(error, "the form-bound Field `" + field.name + "` widget `" +
                                       widget.id + "` sets `" + std::string(owned) +
                                       "`; the Form controller owns the field value");
            }
        }
        if (field.text_widget && widget.events.find("onChange") != widget.events.end()) {
            return fail(error, "the form-bound Field `" + field.name + "` widget `" + widget.id +
                                   "` binds `onChange`; the generated form binding owns the edit "
                                   "event");
        }
    }
    for (const std::string_view owned : {"value", "defaultValue", "error", "errors"}) {
        if (find_prop(node, owned) != nullptr) {
            return fail(error, "the form-bound Field `" + field.name + "` passes `" +
                                   std::string(owned) +
                                   "`; the Form controller resolves that prop by `name`, so the "
                                   "explicit prop is rejected");
        }
    }
    return parse_field_rules(node, field, error);
}

bool parse_form(const ir::Node& node, std::string& error) {
    const ir::Expr* initial = find_prop(node, "initial");
    if (initial == nullptr) {
        return true; // the default `[]`: every field starts empty
    }
    if (initial->kind != ir::ExprKind::Array) {
        return fail(error, "`initial` of the Form instance `" + node.id +
                               "` must be a literal `name=value` string array");
    }
    std::vector<std::string> seen;
    for (const ir::ExprPtr& item : initial->operands) {
        const std::optional<std::string_view> seed = string_literal(item.get());
        if (!seed) {
            return fail(error, "`initial` of the Form instance `" + node.id +
                                   "` must contain string literals only");
        }
        const std::size_t split = seed->find('=');
        if (split == std::string_view::npos || split == 0) {
            return fail(error, "the Form seed `" + std::string(*seed) +
                                   "` is not `name=value`; an empty or missing name has no field");
        }
        const std::string name(seed->substr(0, split));
        for (const std::string& existing : seen) {
            if (existing == name) {
                return fail(error, "the Form instance `" + node.id + "` seeds `" + name +
                                       "` more than once");
            }
        }
        seen.push_back(name);
    }
    return true;
}

const ir::Component* find_module_component(const ir::IrModule& module, std::string_view id) {
    for (const ir::Component& component : module.components) {
        if (component.id == id) {
            return &component;
        }
    }
    return nullptr;
}

// Pre-order walk with the same ordinal numbering as collect_component_instances(). `current` is the
// nearest enclosing library Form, or nullptr outside one.
struct FormWalk {
    const ir::IrModule& module;
    FormPlan& plan;
    std::string& error;
    unsigned ordinal = 0;
    FormBinding* current = nullptr;

    bool walk(const ir::Node& node);
};

bool FormWalk::walk(const ir::Node& node) {
    const unsigned self = ++ordinal;
    FormBinding* enclosing = current;
    if (node.kind == ir::NodeKind::Component) {
        const ir::Component* target = find_module_component(module, node.component);
        if (target == nullptr) {
            return true; // the composition path reports the unknown component
        }
        if (is_library_component(*target, "Form")) {
            if (current != nullptr) {
                return fail(error, "a Form (`" + node.id +
                                       "`) inside another Form is deferred; use one Form per "
                                       "field set");
            }
            FormBinding form;
            form.ordinal = self;
            form.symbol = form_member(self);
            if (!parse_form(node, error)) {
                return false;
            }
            plan.forms.push_back(std::move(form));
            enclosing = &plan.forms.back();
        } else if (is_library_component(*target, "Field") && current != nullptr) {
            FormFieldBinding field;
            if (!parse_field(node, self, field, error)) {
                return false;
            }
            field.form_ordinal = current->ordinal;
            for (const FormFieldBinding& existing : current->fields) {
                if (existing.name == field.name) {
                    return fail(error, "two Fields inside the Form instance name `" + field.name +
                                           "`; field names are stable paths and must be unique");
                }
            }
            current->fields.push_back(std::move(field));
        }
    }
    FormBinding* const saved = current;
    current = enclosing;
    for (const ir::Node& child : node.children) {
        if (!walk(child)) {
            return false;
        }
    }
    if (node.fallback && !walk(*node.fallback)) {
        return false;
    }
    if (node.item_template && !walk(*node.item_template)) {
        return false;
    }
    current = saved;
    return true;
}

} // namespace

bool is_library_component(const ir::Component& component, std::string_view name) {
    if (component.name != name) {
        return false;
    }
    if (name == "Field") {
        return component.source.file == kFieldFile;
    }
    if (name == "Form") {
        return component.source.file == kFormFile;
    }
    return false;
}

std::string form_member(unsigned ordinal) {
    return "form_controller_" + std::to_string(ordinal) + "_";
}

std::string form_field_member(unsigned ordinal, std::string_view symbol, std::string_view suffix) {
    return form_member(ordinal) + std::string(suffix) + "_" + std::string(symbol) + "_";
}

std::string form_field_handler(unsigned form_ordinal, std::string_view symbol) {
    return "form_" + std::to_string(form_ordinal) + "_field_" + std::string(symbol) + "_changed";
}

std::string form_field_adapter(unsigned form_ordinal, std::string_view symbol) {
    return form_field_handler(form_ordinal, symbol) + "_adapter";
}

std::string form_symbol(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        const bool word =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        out += word ? c : '_';
    }
    return out;
}

const FormBinding* FormPlan::form(unsigned ordinal) const {
    for (const FormBinding& binding : forms) {
        if (binding.ordinal == ordinal) {
            return &binding;
        }
    }
    return nullptr;
}

const FormFieldBinding* FormPlan::field(unsigned ordinal) const {
    for (const FormBinding& binding : forms) {
        for (const FormFieldBinding& field : binding.fields) {
            if (field.ordinal == ordinal) {
                return &field;
            }
        }
    }
    return nullptr;
}

bool collect_form_plan(const ir::IrModule& module, const ir::Component& component, FormPlan& plan,
                       std::string& error) {
    FormWalk walk{module, plan, error};
    return walk.walk(component.root);
}

} // namespace tessera::compiler::codegen::internal
