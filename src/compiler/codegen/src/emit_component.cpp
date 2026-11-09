// SPDX-License-Identifier: MIT

// Component header/source emission for the first C++ codegen slice (docs/design/codegen.md).
//
// Generated shape (per component):
//   <Name>.hpp  struct <Name> { state/ref/animated/prop fields; build(); handler_*(); <region>();
//   }; <Name>.cpp  build() creating the static node tree, handler bodies, and one protected
//               [DEVELOPER_LOGIC_START: bind::<id>] region per binding.
// Determinism: fields, children, handlers and bindings are emitted in IR order; maps are sorted;
// two-space indent; LF only. Nothing depends on clang-format or the wall clock.

#include "codegen_internal.hpp"
#include "emit_controlled.hpp"
#include "emit_events.hpp"
#include "interpolation.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace tessera::compiler::codegen::internal {
namespace {

bool fail(std::string& error, std::string message) {
    error = "TESSERA_CODEGEN_UNSUPPORTED: " + std::move(message);
    return false;
}

std::string value_repr(const ir::Expr* expr) {
    if (expr == nullptr) {
        return "null";
    }
    if (expr->kind == ir::ExprKind::Literal) {
        return literal_text(expr->literal);
    }
    return "<expr>";
}

std::string node_id_text(const ir::Node& node, unsigned& next_id, std::string& error) {
    if (node.ui_id.empty()) {
        // Slice 10 computes uiId; until then the pre-order index is the deterministic placeholder.
        return "static_cast<tessera::ui::NodeId>(" + std::to_string(next_id++) + "u)";
    }
    for (const char c : node.ui_id) {
        if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
            fail(error, "node `" + node.id + "` has a non-numeric uiId `" + node.ui_id + "`");
            return {};
        }
    }
    ++next_id;
    return "static_cast<tessera::ui::NodeId>(" + node.ui_id + "ull)";
}

// Remaining metadata gaps for this slice: inline style attachment and event dispatch wiring. Both
// are recorded in a deterministic comment (docs/design/compiler.md section 11).
std::string node_comment(const ir::Node& node) {
    std::string out = "// " + node.id;
    for (const auto& [name, value] : node.style) {
        out += " style." + name + "=" + value_repr(value.get());
    }
    if (!node.events.empty()) {
        out += " events={";
        bool first = true;
        for (const auto& [name, target] : node.events) {
            out += (first ? "" : ",") + name + ":" + target;
            first = false;
        }
        out += "}";
    }
    return out;
}

std::optional<std::string_view> literal_string(const ir::Expr* expr) {
    if (expr == nullptr || expr->kind != ir::ExprKind::Literal) {
        return std::nullopt;
    }
    if (const auto* text = std::get_if<std::string>(&expr->literal)) {
        return std::string_view(*text);
    }
    return std::nullopt;
}

std::optional<std::uint64_t> parse_automation_id(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

bool emit_node(CppWriter& writer, const ir::Node& node, const ir::Component& component,
               const std::vector<EventAdapter>& adapters,
               const std::vector<DynamicText>& dynamic_text,
               const std::vector<DynamicMetadata>& dynamic_metadata,
               const std::vector<ControlledInput>& controlled_inputs, unsigned& next_id,
               unsigned& next_var, std::string& variable, std::string& error) {
    switch (node.kind) {
    case ir::NodeKind::CustomElement:
    case ir::NodeKind::Component:
    case ir::NodeKind::VirtualList:
    case ir::NodeKind::VirtualGrid:
        return fail(error, "`" + std::string(ir::node_kind_name(node.kind)) +
                               "` nodes are deferred (component composition, custom elements and "
                               "virtualized lists are later codegen slices)");
    default:
        break;
    }
    if (node.condition) {
        return fail(error,
                    "a conditional node is deferred (conditionals are a later codegen slice)");
    }
    if (node.loop) {
        return fail(error, "a loop template is deferred (list emission is a later codegen slice)");
    }
    // Every expression recorded in this node must have a C++ spelling even though the scene API
    // cannot apply it yet: an unsupported expression is rejected here, never silently dropped.
    for (const auto& [name, value] : node.props) {
        // Element metadata is emitted by the static/dynamic metadata paths below, which support
        // text interpolation; the generic expression validation only covers other props.
        if (name == "id" || name == "className" || name == "role" || name.rfind("aria-", 0) == 0) {
            continue;
        }
        if (value) {
            const ExprEmission emission = emit_expr(*value, component);
            if (!emission.error.empty()) {
                return fail(error, "prop `" + name + "`: " + emission.error);
            }
        }
    }
    for (const auto& [name, value] : node.style) {
        if (value) {
            const ExprEmission emission = emit_expr(*value, component);
            if (!emission.error.empty()) {
                return fail(error, "style `" + name + "`: " + emission.error);
            }
        }
    }
    if (node.key) {
        const ExprEmission emission = emit_expr(*node.key, component);
        if (!emission.error.empty()) {
            return fail(error, "key: " + emission.error);
        }
    }

    const std::string id = node_id_text(node, next_id, error);
    if (!error.empty()) {
        return false;
    }
    writer.line(node_comment(node));
    const unsigned node_ordinal = next_var;
    const bool dynamic =
        std::any_of(dynamic_text.begin(), dynamic_text.end(),
                    [&](const DynamicText& entry) { return entry.ordinal == node_ordinal; });
    variable = "node_" + std::to_string(next_var++);
    writer.line("auto " + variable + " = std::make_unique<tessera::ui::Node>(" + id + ");");
    // Deterministic metadata order: element kind, id, className, automationId, role, aria-* (name
    // order), event bindings, focusability/scroll flags, paint text, then the child subtrees.
    const auto prop = [&](std::string_view name) -> const ir::Expr* {
        const auto found = node.props.find(std::string(name));
        return found == node.props.end() ? nullptr : found->second.get();
    };
    const auto is_dynamic = [&](MetadataField field, std::string_view name) {
        return std::any_of(
            dynamic_metadata.begin(), dynamic_metadata.end(), [&](const DynamicMetadata& entry) {
                return entry.ordinal == node_ordinal && entry.field == field && entry.name == name;
            });
    };
    writer.line(variable + "->set_element_kind(\"" + std::string(ir::node_kind_name(node.kind)) +
                "\");");
    for (const DynamicMetadata& entry : dynamic_metadata) {
        if (entry.ordinal == node_ordinal) {
            writer.line(metadata_member(entry) + "_node_ = " + variable + ".get();");
        }
    }
    if (const ir::Expr* id_prop = prop("id")) {
        const std::optional<std::string_view> text = literal_string(id_prop);
        if (text) {
            writer.line(variable + "->set_element_id(" +
                        literal_text(ir::LiteralValue{std::string(*text)}) + ");");
        } else if (!is_dynamic(MetadataField::ElementId, "id")) {
            return fail(error, "id: a dynamic element id has no printable type");
        }
    }
    if (const ir::Expr* class_prop = prop("className")) {
        const std::optional<std::string_view> text = literal_string(class_prop);
        if (text) {
            writer.line(variable + "->set_class_name(" +
                        literal_text(ir::LiteralValue{std::string(*text)}) + ");");
        } else if (!is_dynamic(MetadataField::ClassName, "className")) {
            return fail(error, "className: a dynamic class has no printable type");
        }
    }
    if (!node.automation_id.empty()) {
        const std::optional<std::uint64_t> value = parse_automation_id(node.automation_id);
        if (!value) {
            return fail(error, "node `" + node.id + "` has a non-numeric automationId `" +
                                   node.automation_id + "`");
        }
        writer.line(variable + "->set_automation_id(" + std::to_string(*value) + "ull);");
    }
    if (const ir::Expr* role_prop = prop("role")) {
        const std::optional<std::string_view> text = literal_string(role_prop);
        if (text) {
            writer.line(variable + "->set_role(" +
                        literal_text(ir::LiteralValue{std::string(*text)}) + ");");
        } else if (!is_dynamic(MetadataField::Role, "role")) {
            return fail(error, "role: a dynamic role has no printable type");
        }
    }
    for (const auto& [name, value] : node.props) {
        if (name.rfind("aria-", 0) != 0) {
            continue;
        }
        const std::optional<std::string_view> text = literal_string(value.get());
        if (text) {
            writer.line(variable + "->set_aria(" +
                        literal_text(ir::LiteralValue{std::string(name)}) + ", " +
                        literal_text(ir::LiteralValue{std::string(*text)}) + ");");
        } else if (!is_dynamic(MetadataField::Aria, name)) {
            return fail(error, name + ": a dynamic aria value has no printable type");
        }
    }
    const ControlledInput* controlled_input = nullptr;
    for (const ControlledInput& input : controlled_inputs) {
        if (input.ordinal == node_ordinal) {
            controlled_input = &input;
            break;
        }
    }
    for (const auto& [jsx_name, target] : node.events) {
        (void)target;
        if (controlled_input != nullptr && jsx_name == "onChange") {
            // Wired through TextChangeBinding below, not the generic scene-event adapter.
            continue;
        }
        const EventAdapter* adapter = nullptr;
        for (const EventAdapter& candidate : adapters) {
            if (candidate.ordinal == node_ordinal && candidate.jsx_name == jsx_name) {
                adapter = &candidate;
                break;
            }
        }
        if (adapter == nullptr) {
            return fail(error, "event `" + jsx_name + "` has no adapter");
        }
        writer.line(variable + "->bind_event(" + adapter->event_type + ", &" + adapter->function +
                    ", this);");
    }
    if (controlled_input != nullptr) {
        emit_controlled_wiring(writer, variable, *controlled_input);
    }
    for (const ir::Ref& ref : component.refs) {
        if (ref.target == node.id) {
            writer.line("this->" + member_name(ref.id) + " = " + variable + ".get();");
        }
    }
    if (node.kind == ir::NodeKind::Button || node.kind == ir::NodeKind::TextInput ||
        node.kind == ir::NodeKind::TextArea) {
        writer.line(variable + "->set_focusable(true);");
    }
    if (node.kind == ir::NodeKind::Scroll) {
        std::string axis = "y";
        const auto found = node.props.find("axis");
        if (found != node.props.end() && found->second &&
            found->second->kind == ir::ExprKind::Literal) {
            if (const auto* text = std::get_if<std::string>(&found->second->literal)) {
                axis = *text;
            }
        }
        writer.line(variable + "->set_scrollable_x(" +
                    (axis == "x" || axis == "both" ? "true" : "false") + ");");
        writer.line(variable + "->set_scrollable_y(" +
                    (axis == "y" || axis == "both" ? "true" : "false") + ");");
    }
    const ir::Expr* paint_expr = node.text.get();
    if (paint_expr == nullptr && controlled_input != nullptr) {
        paint_expr = controlled_input->value;
    }
    if (const std::optional<std::string> paint_text = static_text_value(paint_expr)) {
        // D9 paint payload: text is visual only (layout/wrap stay paint-side).
        const std::string paint = "paint_" + variable;
        writer.line("tessera::ui::PaintData " + paint + ";");
        writer.line(paint + ".text = " + literal_text(ir::LiteralValue{std::string(*paint_text)}) +
                    ";");
        writer.line(variable + "->set_paint_data(std::move(" + paint + "));");
        if (controlled_input != nullptr) {
            writer.line("this->" + controlled_input->editor_member + ".set_text(" +
                        literal_text(ir::LiteralValue{std::string(*paint_text)}) + ");");
        }
    } else if (dynamic) {
        const std::string suffix = std::to_string(node_ordinal);
        writer.line("this->text_node_" + suffix + "_ = " + variable + ".get();");
    }
    for (const ir::Node& child : node.children) {
        std::string child_variable;
        if (!emit_node(writer, child, component, adapters, dynamic_text, dynamic_metadata,
                       controlled_inputs, next_id, next_var, child_variable, error)) {
            return false;
        }
        writer.line(variable + "->add_child(std::move(" + child_variable + "));");
    }
    return true;
}

std::vector<const ir::Binding*> component_bindings(const ir::Component& component,
                                                   const std::vector<ir::Binding>& bindings) {
    std::vector<const ir::Binding*> result;
    for (const ir::Binding& binding : bindings) {
        if (binding.component == component.id) {
            result.push_back(&binding);
        }
    }
    return result;
}

bool emit_stub_body(CppWriter& writer, const ir::Binding& binding,
                    std::string_view event_parameter) {
    writer.line("// TODO(tessera-codegen): implement the developer body for " + binding.id + ".");
    if (!event_parameter.empty()) {
        writer.line("(void)" + std::string(event_parameter) + ";");
    }
    if (!binding.return_type.empty()) {
        writer.line("return {};");
    }
    return true;
}

bool emit_source(const ir::Component& component, std::string_view ns,
                 const std::vector<const ir::Binding*>& bindings,
                 const std::vector<DynamicText>& dynamic_text,
                 const std::vector<DynamicMetadata>& dynamic_metadata,
                 const std::vector<ControlledInput>& controlled_inputs, GeneratedFile& file,
                 std::string& error) {
    CppWriter writer;
    writer.line("// SPDX-License-Identifier: MIT");
    writer.line("// Generated by Tessera codegen from the IR; see docs/design/codegen.md.");
    writer.line("// Developer code belongs only inside [DEVELOPER_LOGIC_START/END] regions.");
    writer.line("#include \"" + component.name + ".hpp\"");
    writer.line("");
    writer.line("#include <memory>");
    writer.line("#include <utility>");
    if (!dynamic_text.empty() || !dynamic_metadata.empty()) {
        writer.line("#include <charconv>");
        writer.line("#include <cstdint>");
        writer.line("#include <string>");
        writer.line("#include <string_view>");
    }
    writer.line("");
    writer.open("namespace " + std::string(ns));
    writer.open("namespace gen");
    writer.line(
        "// TODO(tessera-codegen): the remaining runtime API gap is inline style attachment");
    writer.line("// (docs/design/compiler.md section 11); style values are recorded in the node");
    writer.line("// comments below until it lands.");
    writer.line("");
    if (!dynamic_text.empty() || !dynamic_metadata.empty()) {
        emit_format_helpers(writer);
    }
    unsigned ordinal = 0;
    std::vector<EventAdapter> adapters;
    if (!collect_event_adapters(component.root, component, bindings, controlled_inputs, ordinal,
                                adapters, error)) {
        return false;
    }
    emit_event_adapters(writer, component.name, adapters);
    emit_controlled_adapters(writer, component.name, controlled_inputs);
    writer.open("void " + component.name + "::build(tessera::ui::Node& parent)");
    unsigned next_id = 1;
    unsigned next_var = 1;
    std::string root_variable;
    if (!emit_node(writer, component.root, component, adapters, dynamic_text, dynamic_metadata,
                   controlled_inputs, next_id, next_var, root_variable, error)) {
        return false;
    }
    writer.line("parent.add_child(std::move(" + root_variable + "));");
    if (!dynamic_text.empty() || !dynamic_metadata.empty()) {
        writer.line("apply_state();");
    }
    writer.close("");
    writer.line("");
    if (!dynamic_text.empty() || !dynamic_metadata.empty()) {
        if (!emit_apply_state(writer, component, dynamic_text, dynamic_metadata, controlled_inputs,
                              error)) {
            return false;
        }
    } else {
        writer.open("void " + component.name + "::apply_state()");
        writer.close("");
        writer.line("");
    }
    if (!emit_lifecycle(component, bindings, writer, error)) {
        return false;
    }
    emit_controlled_accessors(writer, component, controlled_inputs);
    for (const ir::Handler& handler : component.handlers) {
        bool typed = false;
        std::string param_name;
        const std::optional<std::string> signature =
            payload_signature(handler.params, typed, param_name);
        if (!signature) {
            return fail(error, "handler `" + handler.id + "` has an unsupported payload record");
        }
        const std::string name = method_name(handler.id);
        writer.open("void " + component.name + "::" + name + "(" + *signature + ")");
        writer.line("(void)" + param_name + ";");
        std::string action_error;
        const std::optional<std::string> body =
            emit_actions(handler.actions, component, handler.params, action_error);
        if (!body) {
            return fail(error, action_error);
        }

        std::string_view rest = *body;
        while (!rest.empty()) {
            const std::size_t newline = rest.find('\n');
            const std::string_view line = rest.substr(0, newline);
            if (!line.empty()) {
                writer.line(line);
            }
            if (newline == std::string_view::npos) {
                break;
            }
            rest = rest.substr(newline + 1);
        }
        if (!dynamic_text.empty() || !dynamic_metadata.empty()) {
            writer.line("apply_state();");
        }
        writer.close("");
        writer.line("");
    }
    for (const ir::Binding* binding : bindings) {
        const bool event_parameter = binding->kind == ir::BindingKind::Event;
        std::string signature;
        std::string param_name;
        if (event_parameter) {
            bool typed = false;
            const std::optional<std::string> payload =
                payload_signature(binding->params, typed, param_name);
            if (!payload) {
                return fail(error,
                            "binding `" + binding->id + "` has an unsupported payload record");
            }
            signature = "void " + component.name + "::" + binding->region + "(" + *payload + ")";
        } else if (!binding->return_type.empty()) {
            const std::string type = handler_type(binding->return_type);
            signature = type + " " + component.name + "::" + binding->region + "()";
        } else {
            signature = "void " + component.name + "::" + binding->region + "()";
        }
        writer.open(signature);
        writer.line("// [DEVELOPER_LOGIC_START: " + binding->id + "]");
        writer.indent += 1;
        emit_stub_body(writer, *binding, event_parameter ? param_name : std::string());
        writer.indent -= 1;
        writer.line("// [DEVELOPER_LOGIC_END: " + binding->id + "]");
        if ((!dynamic_text.empty() || !dynamic_metadata.empty()) && event_parameter &&
            binding->return_type.empty()) {
            writer.line("apply_state();");
        }
        writer.close("");
        writer.line("");
    }
    writer.close("");
    writer.close("");
    file.path = component.name + ".cpp";
    file.text = std::move(writer.text);
    return true;
}

} // namespace

bool emit_component(const ir::Component& component, std::string_view module_namespace,
                    const std::vector<ir::Binding>& bindings, std::vector<GeneratedFile>& files,
                    std::string& error) {
    if (component.name.empty()) {
        return fail(error, "a component has no name");
    }
    if (component.environment.size() != 0) {
        return fail(error, "component `" + component.name +
                               "` uses library hooks (`environment`), which are a later slice");
    }
    const std::vector<const ir::Binding*> owned = component_bindings(component, bindings);
    unsigned ordinal = 0;
    std::vector<DynamicText> dynamic_text;
    if (!collect_dynamic_text(component.root, component, ordinal, dynamic_text, error)) {
        return false;
    }
    ordinal = 0;
    std::vector<DynamicMetadata> dynamic_metadata;
    if (!collect_dynamic_metadata(component.root, component, ordinal, dynamic_metadata, error)) {
        return false;
    }
    ordinal = 0;
    std::vector<ControlledInput> controlled_inputs;
    if (!collect_controlled_inputs(component.root, component, owned, ordinal, controlled_inputs,
                                   error)) {
        return false;
    }
    GeneratedFile header;
    if (!emit_component_header(component, module_namespace, owned, dynamic_text, dynamic_metadata,
                               controlled_inputs, header, error)) {
        return false;
    }
    GeneratedFile source;
    if (!emit_source(component, module_namespace, owned, dynamic_text, dynamic_metadata,
                     controlled_inputs, source, error)) {
        return false;
    }
    files.push_back(std::move(header));
    files.push_back(std::move(source));
    return true;
}

} // namespace tessera::compiler::codegen::internal