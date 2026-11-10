// SPDX-License-Identifier: MIT

// Controlled TextInput/TextArea emission: value -> PaintData + editor buffer, typed onChange.

#include "emit_controlled.hpp"
#include "emit_events.hpp"
#include "interpolation.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::codegen::internal {
namespace {

bool fail(std::string& error, std::string message) {
    error = "TESSERA_CODEGEN_UNSUPPORTED: " + std::move(message);
    return false;
}

const ir::Handler* find_handler(const ir::Component& component, std::string_view id) {
    for (const ir::Handler& handler : component.handlers) {
        if (handler.id == id) {
            return &handler;
        }
    }
    return nullptr;
}

const ir::Binding* find_binding(const std::vector<const ir::Binding*>& bindings,
                                std::string_view id) {
    for (const ir::Binding* binding : bindings) {
        if (binding->id == id) {
            return binding;
        }
    }
    return nullptr;
}

const ir::Expr* prop_of(const ir::Node& node, std::string_view name) {
    const auto found = node.props.find(std::string(name));
    return found == node.props.end() ? nullptr : found->second.get();
}

std::string suffix(std::string_view prefix, unsigned ordinal) {
    return std::string(prefix) + std::to_string(ordinal) + "_";
}

} // namespace

bool collect_controlled_inputs(const ir::Node& node, const ir::Component& component,
                               const std::vector<const ir::Binding*>& bindings, unsigned& ordinal,
                               std::vector<ControlledInput>& inputs, std::string& error) {
    ++ordinal;
    const bool text_field =
        node.kind == ir::NodeKind::TextInput || node.kind == ir::NodeKind::TextArea;
    if (text_field) {
        const ir::Expr* value = prop_of(node, "value");
        if (value == nullptr) {
            value = prop_of(node, "defaultValue");
        }
        if (value != nullptr) {
            ControlledInput input;
            input.ordinal = ordinal;
            input.value = value;
            input.dynamic = !static_text_value(value).has_value();
            input.value_member = "text_node_" + std::to_string(ordinal) + "_value_";
            input.editor_member = suffix("input_", ordinal) + "editor_";
            input.runtime_member = suffix("input_", ordinal) + "runtime_";
            input.binding_member = suffix("input_", ordinal) + "change_binding_";
            input.adapter = "text_change_adapter_" + std::to_string(ordinal);
            if (!printable_text_expression(*value, component)) {
                return fail(error, "the `value` of `" + node.id +
                                       "` has no printable text type for a controlled field");
            }
            const auto on_change = node.events.find("onChange");
            if (on_change != node.events.end()) {
                const std::vector<ir::Param>* params = nullptr;
                if (on_change->second.rfind("handler::", 0) == 0) {
                    const ir::Handler* handler = find_handler(component, on_change->second);
                    if (handler == nullptr) {
                        return fail(error,
                                    "onChange names unknown handler `" + on_change->second + "`");
                    }
                    params = &handler->params;
                    input.method = method_name(handler->id);
                } else if (on_change->second.rfind("bind::", 0) == 0) {
                    const ir::Binding* binding = find_binding(bindings, on_change->second);
                    if (binding == nullptr) {
                        return fail(error,
                                    "onChange names unknown binding `" + on_change->second + "`");
                    }
                    params = &binding->params;
                    input.method = binding->region;
                } else {
                    return fail(error, "onChange names neither a handler nor a binding");
                }
                if (params->empty() || params->front().type != "TextChangeEvent") {
                    return fail(error, "onChange of `" + node.id +
                                           "` must bind a TextChangeEvent payload; the runtime "
                                           "change adapter is the only supported form");
                }
            }
            inputs.push_back(std::move(input));
        }
    }
    for (const ir::Node& child : node.children) {
        if (!collect_controlled_inputs(child, component, bindings, ordinal, inputs, error)) {
            return false;
        }
    }
    // A controlled field inside a virtual item template is rejected by the plan; the recursion
    // keeps the pre-order ordinals of the nodes after the collection aligned.
    if (node.item_template && !collect_controlled_inputs(*node.item_template, component, bindings,
                                                         ordinal, inputs, error)) {
        return false;
    }
    return true;
}

void emit_controlled_fields(CppWriter& writer, const std::vector<ControlledInput>& inputs) {
    if (inputs.empty()) {
        return;
    }
    for (const ControlledInput& input : inputs) {
        writer.line("// Controlled text field: value -> PaintData, committed edits -> onChange.");
        writer.line("tessera::ui::text::EditController " + input.editor_member + ";");
        writer.line("tessera::ui::text::TextInputRuntime " + input.runtime_member + "{" +
                    input.editor_member + "};");
        writer.line("tessera::ui::TextChangeBinding " + input.binding_member + ";");
    }
    writer.line("");
    writer.line("// The generated editor of the Nth controlled field (decoration/tests).");
    writer.line("[[nodiscard]] std::size_t text_input_count() const {");
    ++writer.indent;
    writer.line("return " + std::to_string(inputs.size()) + "u;");
    --writer.indent;
    writer.line("}");
    writer.line("[[nodiscard]] tessera::ui::text::EditController& text_input_editor(std::size_t "
                "index);");
    writer.line("[[nodiscard]] std::size_t text_input_change_events(std::size_t index) const;");
}

void emit_controlled_adapters(CppWriter& writer, std::string_view component_name,
                              const std::vector<ControlledInput>& inputs) {
    if (inputs.empty()) {
        return;
    }
    writer.open("namespace");
    for (const ControlledInput& input : inputs) {
        if (input.method.empty()) {
            continue;
        }
        writer.open("void " + input.adapter +
                    "(void* context, const tessera::ui::TextChangeEvent& event)");
        writer.line("auto* component = static_cast<" + std::string(component_name) +
                    "*>(context);");
        writer.line("component->" + input.method + "(event);");
        writer.close("");
    }
    writer.close("");
    writer.line("");
}

void emit_controlled_wiring(CppWriter& writer, const std::string& variable,
                            const ControlledInput& input) {
    writer.line("this->" + input.runtime_member + ".attach(" + variable + ".get());");
    writer.line("tessera::ui::text::bind_text_input_events(*" + variable + ", this->" +
                input.runtime_member + ");");
    if (!input.method.empty()) {
        writer.line("this->" + input.binding_member + ".handler = &" + input.adapter + ";");
        writer.line("this->" + input.binding_member + ".context = this;");
        writer.line("tessera::ui::bind_text_change(*" + variable + ", this->" +
                    input.binding_member + ");");
    }
}

void emit_controlled_apply(CppWriter& writer, const std::vector<ControlledInput>& inputs) {
    for (const ControlledInput& input : inputs) {
        if (!input.dynamic) {
            continue;
        }
        writer.open("if (" + input.editor_member + ".buffer().text() != " + input.value_member +
                    ")");
        ++writer.indent;
        writer.line(input.editor_member + ".set_text(" + input.value_member + ");");
        --writer.indent;
        writer.close("");
    }
}

void emit_controlled_accessors(CppWriter& writer, const ir::Component& component,
                               const std::vector<ControlledInput>& inputs) {
    if (inputs.empty()) {
        return;
    }
    writer.open("tessera::ui::text::EditController& " + component.name +
                "::text_input_editor(std::size_t index)");
    writer.line("switch (index) {");
    ++writer.indent;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        writer.line("case " + std::to_string(i) + "u:");
        ++writer.indent;
        writer.line("return " + inputs[i].editor_member + ";");
        --writer.indent;
    }
    --writer.indent;
    writer.line("}");
    writer.line("return " + inputs.front().editor_member + ";");
    writer.close("");
    writer.line("");
    writer.open("std::size_t " + component.name +
                "::text_input_change_events(std::size_t index) const");
    writer.line("switch (index) {");
    ++writer.indent;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        writer.line("case " + std::to_string(i) + "u:");
        ++writer.indent;
        writer.line("return " + inputs[i].runtime_member + ".change_events();");
        --writer.indent;
    }
    --writer.indent;
    writer.line("}");
    writer.line("return " + inputs.front().runtime_member + ".change_events();");
    writer.close("");
    writer.line("");
}

} // namespace tessera::compiler::codegen::internal
