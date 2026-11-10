// SPDX-License-Identifier: MIT

// Generated form-state binding emission (docs/specs/forms-and-validation-v1.md section 5.2,
// docs/design/compiler.md section 11). The library `Form` component stays presentational; the
// caller that owns the instance owns the `tessera::ui::forms::Form` controller, because the Field
// instances are caller-owned slot content. Every generated assignment is value-guarded: an update
// copies a string only when the controller value/error actually changed.

#include "emit_forms.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace tessera::compiler::codegen::internal {
namespace {

std::string form_method(unsigned ordinal, std::string_view method) {
    return std::string(method) + "_" + std::to_string(ordinal);
}

std::string field_handler(unsigned ordinal, const FormFieldBinding& field) {
    return form_field_handler(ordinal, field.symbol);
}

std::string field_adapter(unsigned ordinal, const FormFieldBinding& field) {
    return form_field_adapter(ordinal, field.symbol);
}

const FormFieldBinding* first_text_field(const FormBinding& form) {
    for (const FormFieldBinding& field : form.fields) {
        if (field.text_widget) {
            return &field;
        }
    }
    return nullptr;
}

void add_field_line(CppWriter& writer, const FormBinding& form, const FormFieldBinding& field) {
    const std::string member = form_field_member(form.ordinal, field.symbol, "id");
    writer.line("if (" + member + " == tessera::ui::forms::kInvalidFieldId) {");
    ++writer.indent;
    writer.line(member + " = this->" + form.symbol + "controller_->add_field(\"" + field.name +
                "\", tessera::ui::forms::FormValue::from_string(\"\"))");
    writer.line("    .value_or(tessera::ui::forms::kInvalidFieldId);");
    --writer.indent;
    writer.line("}");
}

void emit_seed_field(CppWriter& writer, const FormBinding& form, const FormFieldBinding& field) {
    const std::string member = form_field_member(form.ordinal, field.symbol, "id");
    writer.line("if (name == \"" + field.name + "\") {");
    ++writer.indent;
    writer.line(member + " = id;");
    --writer.indent;
    writer.line("}");
}

void emit_rule(CppWriter& writer, const FormBinding& form, const FormFieldBinding& field) {
    const std::string id = form_field_member(form.ordinal, field.symbol, "id");
    for (const FormRule& rule : field.rules) {
        writer.line("if (" + id + " != tessera::ui::forms::kInvalidFieldId) {");
        ++writer.indent;
        writer.line("tessera::ui::forms::SyncRule rule;");
        switch (rule.kind) {
        case FormRule::Kind::Required:
            writer.line("rule.kind = tessera::ui::forms::RuleKind::Required;");
            break;
        case FormRule::Kind::MinLength:
            writer.line("rule.kind = tessera::ui::forms::RuleKind::MinLength;");
            writer.line("rule.minimum = " + std::to_string(rule.number) + ";");
            break;
        case FormRule::Kind::MaxLength:
            writer.line("rule.kind = tessera::ui::forms::RuleKind::MaxLength;");
            writer.line("rule.maximum = " + std::to_string(rule.number) + ";");
            break;
        case FormRule::Kind::Pattern:
            writer.line("rule.kind = tessera::ui::forms::RuleKind::Pattern;");
            writer.line("rule.pattern = " +
                        literal_text(ir::LiteralValue{std::string(rule.pattern)}) + ";");
            break;
        }
        writer.line("this->" + form.symbol + "controller_->add_rule(" + id + ", rule);");
        --writer.indent;
        writer.line("}");
    }
}

void emit_field_refresh(CppWriter& writer, const FormBinding& form, const FormFieldBinding& field) {
    const std::string member = "this->" + form_field_member(form.ordinal, field.symbol, "id");
    writer.line("if (const tessera::ui::forms::FieldState* state =");
    writer.line("        this->form_" + std::to_string(form.ordinal) + "().field(" + member +
                ")) {");
    ++writer.indent;
    const std::string instance = "this->" + instance_member(field.ordinal) + ".";
    writer.line("if (" + instance + "prop_value_ != state->value.text) {");
    ++writer.indent;
    writer.line(instance + "prop_value_ = state->value.text;");
    --writer.indent;
    writer.line("}");
    writer.line("// Surface issues only after interaction or an explicit validate/submit.");
    writer.line("const bool show_errors = state->touched || state->dirty ||");
    writer.line("    this->" + form.symbol + "errors_visible_;");
    writer.line("std::string first_error;");
    writer.line("if (show_errors) {");
    ++writer.indent;
    writer.line("for (const tessera::ui::forms::FormIssue& issue : state->issues) {");
    ++writer.indent;
    writer.line("if (issue.severity == tessera::ui::forms::IssueSeverity::Error) {");
    ++writer.indent;
    writer.line("first_error = issue.message;");
    writer.line("break;");
    --writer.indent;
    writer.line("}");
    --writer.indent;
    writer.line("}");
    --writer.indent;
    writer.line("}");
    writer.line("if (" + instance + "prop_error_ != first_error) {");
    ++writer.indent;
    writer.line(instance + "prop_error_ = std::move(first_error);");
    --writer.indent;
    writer.line("}");
    writer.line("std::vector<std::string> errors;");
    writer.line("if (show_errors) {");
    ++writer.indent;
    writer.line("errors.reserve(state->issues.size());");
    writer.line("for (const tessera::ui::forms::FormIssue& issue : state->issues) {");
    ++writer.indent;
    writer.line("errors.push_back(issue.message);");
    --writer.indent;
    writer.line("}");
    --writer.indent;
    writer.line("}");
    writer.line("if (" + instance + "prop_errors_ != errors) {");
    ++writer.indent;
    writer.line(instance + "prop_errors_ = std::move(errors);");
    --writer.indent;
    writer.line("}");
    --writer.indent;
    writer.line("}");
}

void emit_field_repaint(CppWriter& writer, const FormBinding& form) {
    for (const FormFieldBinding& field : form.fields) {
        writer.line("this->" + instance_member(field.ordinal) + ".apply_state();");
    }
}

} // namespace

void emit_form_includes(CppWriter& writer, const FormPlan& plan) {
    if (plan.empty()) {
        return;
    }
    writer.line("#include \"tessera/ui/forms/form_model.hpp\"");
    writer.line("#include \"tessera/ui/scene/focus.hpp\"");
}

void emit_form_declarations(CppWriter& writer, const FormPlan& plan) {
    for (const FormBinding& form : plan.forms) {
        writer.line("// Form instance (pre-order index " + std::to_string(form.ordinal) +
                    "): the generated form-state binding (forms v1 section 5.2).");
        writer.line(
            "// The controller is built on first use so the host can set the config (queue).");
        writer.line("tessera::ui::forms::FormConfig " + form.symbol + "config_;");
        writer.line("std::optional<tessera::ui::forms::Form> " + form.symbol + "controller_;");
        writer.line("bool " + form.symbol + "seeded_ = false;");
        writer.line("tessera::ui::forms::SubmitResult " + form.symbol + "last_submit_;");
        writer.line("std::string " + form.symbol + "report_;");
        writer.line("tessera::ui::FocusManager* " + form.symbol + "focus_ = nullptr;");
        writer.line(
            "// Errors are surfaced after the first interaction or an explicit validate/submit;");
        writer.line("// a pristine form does not show its Required rule (forms v1 section 2).");
        writer.line("bool " + form.symbol + "errors_visible_ = false;");
        for (const FormFieldBinding& field : form.fields) {
            writer.line("// Field `" + field.name + "` (pre-order index " +
                        std::to_string(field.ordinal) + ").");
            writer.line("tessera::ui::forms::FieldId " +
                        form_field_member(form.ordinal, field.symbol, "id") +
                        " = tessera::ui::forms::kInvalidFieldId;");
            writer.line("tessera::ui::Node* " +
                        form_field_member(form.ordinal, field.symbol, "widget") + " = nullptr;");
        }
    }
    if (plan.empty()) {
        return;
    }
    writer.line("");
    writer.line(
        "// Form binding: seed the controller from `initial`, resolve each Field by `name`,");
    writer.line(
        "// then route validation/submit to the controller. The library evaluates the rules.");
    writer.line("void set_form_focus(tessera::ui::FocusManager* manager);");
    for (const FormBinding& form : plan.forms) {
        writer.line("tessera::ui::forms::Form& form_" + std::to_string(form.ordinal) + "();");
        writer.line("void " + form_method(form.ordinal, "seed_form") + "();");
        writer.line("void " + form_method(form.ordinal, "apply_form_fields") + "();");
        writer.line("bool " + form_method(form.ordinal, "validate_form") + "();");
        writer.line("tessera::ui::forms::SubmitResult " + form_method(form.ordinal, "submit_form") +
                    "();");
        for (const FormFieldBinding& field : form.fields) {
            if (field.text_widget) {
                writer.line("void " + field_handler(form.ordinal, field) +
                            "(const tessera::ui::TextChangeEvent& event);");
            }
        }
    }
}

void emit_form_build_calls(CppWriter& writer, const FormPlan& plan) {
    for (const FormBinding& form : plan.forms) {
        writer.line(form_method(form.ordinal, "seed_form") + "();");
        writer.line(form_method(form.ordinal, "apply_form_fields") + "();");
    }
}

void emit_form_state_calls(CppWriter& writer, const FormPlan& plan) {
    for (const FormBinding& form : plan.forms) {
        writer.line(form_method(form.ordinal, "apply_form_fields") + "();");
    }
}

void emit_form_adapters(CppWriter& writer, const ir::Component& component, const FormPlan& plan) {
    bool any = false;
    for (const FormBinding& form : plan.forms) {
        any = any || first_text_field(form) != nullptr;
    }
    if (!any) {
        return;
    }
    writer.open("namespace");
    bool helper_emitted = false;
    for (const FormBinding& form : plan.forms) {
        for (const FormFieldBinding& field : form.fields) {
            if (!field.text_widget) {
                continue;
            }
            writer.open("void " + field_adapter(form.ordinal, field) +
                        "(void* context, const tessera::ui::Event& event)");
            writer.line("auto* component = static_cast<" + component.name + "*>(context);");
            writer.line("component->" + field_handler(form.ordinal, field) +
                        "(tessera::ui::text_change_event(event));");
            writer.close("");
            if (!helper_emitted) {
                helper_emitted = true;
                writer.line("");
                writer.line(
                    "// Focus the first focusable node of a field's slot content (the input");
                writer.line(
                    "// itself, or the first child of a wrapper); no-op without a manager.");
                writer.open("void focus_generated_field(tessera::ui::FocusManager* manager, "
                            "tessera::ui::Node* node)");
                writer.line("if (manager == nullptr) {");
                ++writer.indent;
                writer.line("return;");
                --writer.indent;
                writer.line("}");
                writer.line("while (node != nullptr && !node->focusable()) {");
                ++writer.indent;
                writer.line("if (node->children().empty()) {");
                ++writer.indent;
                writer.line("return;");
                --writer.indent;
                writer.line("}");
                writer.line("node = node->children().front().get();");
                --writer.indent;
                writer.line("}");
                writer.line("if (node != nullptr) {");
                ++writer.indent;
                writer.line("manager->request_focus(node);");
                --writer.indent;
                writer.line("}");
                writer.close("");
            }
        }
    }
    writer.close("");
    writer.line("");
}

bool emit_form_source(CppWriter& writer, const ir::Component& component, const FormPlan& plan,
                      std::string& error) {
    (void)error; // every binding form was validated by collect_form_plan()
    for (const FormBinding& form : plan.forms) {
        const std::string accessor = "form_" + std::to_string(form.ordinal);
        writer.open("tessera::ui::forms::Form& " + component.name + "::" + accessor + "()");
        writer.line(form_method(form.ordinal, "seed_form") + "();");
        writer.line("return *this->" + form.symbol + "controller_;");
        writer.close("");
        writer.line("");
    }
    for (const FormBinding& form : plan.forms) {
        writer.open("void " + component.name + "::" + form_method(form.ordinal, "seed_form") +
                    "()");
        writer.line("if (this->" + form.symbol + "seeded_) {");
        ++writer.indent;
        writer.line("return;");
        --writer.indent;
        writer.line("}");
        writer.line("this->" + form.symbol + "seeded_ = true;");
        writer.line("this->" + form.symbol + "controller_.emplace(this->" + form.symbol +
                    "config_);");
        writer.line(
            "// `initial` seeds in declaration order; a runtime-built list is validated by");
        writer.line("// the caller (codegen accepts literal `name=value` seeds only).");
        writer.line("for (const std::string& seed : this->" + instance_member(form.ordinal) +
                    ".prop_initial_) {");
        ++writer.indent;
        writer.line("const std::size_t split = seed.find('=');");
        writer.line("if (split == std::string::npos || split == 0) {");
        ++writer.indent;
        writer.line("continue;");
        --writer.indent;
        writer.line("}");
        writer.line("const std::string_view name = std::string_view(seed).substr(0, split);");
        writer.line("const std::string_view value = std::string_view(seed).substr(split + 1);");
        writer.line("const tessera::ui::forms::FieldId id =");
        writer.line(
            "    this->" + form.symbol +
            "controller_->add_field(name, tessera::ui::forms::FormValue::from_string(value))");
        writer.line("        .value_or(tessera::ui::forms::kInvalidFieldId);");
        writer.line("if (id == tessera::ui::forms::kInvalidFieldId) {");
        ++writer.indent;
        writer.line("continue;");
        --writer.indent;
        writer.line("}");
        for (const FormFieldBinding& field : form.fields) {
            emit_seed_field(writer, form, field);
        }
        --writer.indent;
        writer.line("}");
        writer.line("// A Field declared in the tree is created even when `initial` omits it.");
        for (const FormFieldBinding& field : form.fields) {
            add_field_line(writer, form, field);
        }
        writer.line(
            "// Rules are data: the forms library evaluates them (no rule code is generated).");
        for (const FormFieldBinding& field : form.fields) {
            emit_rule(writer, form, field);
        }
        writer.close("");
        writer.line("");

        writer.open("void " + component.name +
                    "::" + form_method(form.ordinal, "apply_form_fields") + "()");
        for (const FormFieldBinding& field : form.fields) {
            writer.line("// Field `" + field.name +
                        "` resolves value/error/errors from the controller.");
            emit_field_refresh(writer, form, field);
        }
        writer.close("");
        writer.line("");

        writer.open("bool " + component.name + "::" + form_method(form.ordinal, "validate_form") +
                    "()");
        writer.line(form_method(form.ordinal, "seed_form") + "();");
        writer.line("this->" + form.symbol + "errors_visible_ = true;");
        writer.line("const bool valid = form_" + std::to_string(form.ordinal) +
                    "().validate_all();");
        writer.line("if (this->" + instance_member(form.ordinal) + ".prop_validate_) {");
        ++writer.indent;
        writer.line("this->" + instance_member(form.ordinal) + ".prop_validate_();");
        --writer.indent;
        writer.line("}");
        writer.line(form_method(form.ordinal, "apply_form_fields") + "();");
        emit_field_repaint(writer, form);
        writer.line("return valid;");
        writer.close("");
        writer.line("");

        writer.open("tessera::ui::forms::SubmitResult " + component.name +
                    "::" + form_method(form.ordinal, "submit_form") + "()");
        writer.line(form_method(form.ordinal, "seed_form") + "();");
        writer.line("this->" + form.symbol + "errors_visible_ = true;");
        writer.line("const tessera::ui::forms::SubmitResult result = form_" +
                    std::to_string(form.ordinal) + "().submit();");
        writer.line("this->" + form.symbol + "last_submit_ = result;");
        writer.line("this->" + form.symbol + "report_.clear();");
        writer.line("if (result.status == tessera::ui::forms::SubmitStatus::Ok) {");
        ++writer.indent;
        writer.line("if (this->" + instance_member(form.ordinal) + ".prop_onSubmit_) {");
        ++writer.indent;
        writer.line("this->" + instance_member(form.ordinal) + ".prop_onSubmit_();");
        --writer.indent;
        writer.line("}");
        --writer.indent;
        writer.line("} else if (result.status == tessera::ui::forms::SubmitStatus::Invalid) {");
        ++writer.indent;
        bool first = true;
        for (const FormFieldBinding& field : form.fields) {
            writer.line(
                (first ? "if (result.first_invalid == " : "} else if (result.first_invalid == ") +
                form_field_member(form.ordinal, field.symbol, "id") + ") {");
            ++writer.indent;
            writer.line("this->" + form.symbol + "report_ = \"" + field.name + "\";");
            writer.line("focus_generated_field(this->" + form.symbol + "focus_,");
            writer.line("    this->" + form_field_member(form.ordinal, field.symbol, "widget") +
                        ");");
            --writer.indent;
            first = false;
        }
        writer.line(first ? "{" : "} else {");
        ++writer.indent;
        writer.line("// No owned field to focus yet (a disabled-only form).");
        writer.line("this->" + form.symbol + "report_.clear();");
        --writer.indent;
        writer.line("}");
        --writer.indent;
        writer.line("}");
        writer.line(form_method(form.ordinal, "apply_form_fields") + "();");
        emit_field_repaint(writer, form);
        writer.line("return result;");
        writer.close("");
        writer.line("");
    }
    if (!plan.empty()) {
        writer.open("void " + component.name +
                    "::set_form_focus(tessera::ui::FocusManager* manager)");
        for (const FormBinding& form : plan.forms) {
            writer.line("this->" + form.symbol + "focus_ = manager;");
        }
        writer.close("");
        writer.line("");
    }
    for (const FormBinding& form : plan.forms) {
        for (const FormFieldBinding& field : form.fields) {
            if (!field.text_widget) {
                continue;
            }
            writer.open("void " + component.name + "::" + field_handler(form.ordinal, field) +
                        "(const tessera::ui::TextChangeEvent& event)");
            writer.line("if (this->" + instance_member(field.ordinal) + ".prop_onValidate_) {");
            ++writer.indent;
            writer.line("this->" + instance_member(field.ordinal) + ".prop_onValidate_(event);");
            --writer.indent;
            writer.line("}");
            writer.line("if (" + form_field_member(form.ordinal, field.symbol, "id") +
                        " != tessera::ui::forms::kInvalidFieldId) {");
            ++writer.indent;
            writer.line("form_" + std::to_string(form.ordinal) + "().set_value(");
            writer.line("    " + form_field_member(form.ordinal, field.symbol, "id") + ",");
            writer.line("    tessera::ui::forms::FormValue::from_string(event.value));");
            --writer.indent;
            writer.line("}");
            writer.line(form_method(form.ordinal, "apply_form_fields") + "();");
            writer.line("this->" + instance_member(field.ordinal) + ".apply_state();");
            writer.close("");
            writer.line("");
        }
    }
    return true;
}

} // namespace tessera::compiler::codegen::internal
