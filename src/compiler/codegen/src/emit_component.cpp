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

constexpr std::string_view kUnsupported = "TESSERA_CODEGEN_UNSUPPORTED: ";
constexpr std::string_view kEventType = "const tessera::ui::Event&";

constexpr std::string_view kPayloadRecords[] = {
    "PointerEvent",     "ClickEvent",      "KeyEvent",   "FocusEvent",
    "TextChangeEvent",  "TextSubmitEvent", "TextTarget", "TextSelection",
    "CompositionEvent", "ScrollEvent",     "ErrorInfo",
};

bool is_payload_record(std::string_view type) {
    for (const std::string_view record : kPayloadRecords) {
        if (record == type) {
            return true;
        }
    }
    return false;
}

bool fail(std::string& error, std::string message) {
    error = std::string(kUnsupported) + std::move(message);
    return false;
}

// A binding's developer function; a payload record parameter maps to the closest existing runtime
// event type (`tessera::ui::Event`), which is the missing-API seam documented in the design note.
std::string handler_type(std::string_view type) {
    if (is_payload_record(type)) {
        return std::string(kEventType);
    }
    const std::optional<std::string> mapped = cpp_type(type);
    return mapped ? *mapped : std::string();
}

std::string params_text(const std::vector<ir::Param>& params) {
    if (params.empty()) {
        return std::string(kEventType) + " event";
    }
    std::string out;
    for (const ir::Param& param : params) {
        const std::string type = handler_type(param.type);
        if (!out.empty()) {
            out += ", ";
        }
        out += type.empty() ? std::string("const tessera::ui::Event&") : type;
        out += " " + param.name;
    }
    return out;
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

// `handler::1` -> `handler_1`: the generated member name of an IR record id.
std::string method_name(std::string_view id) {
    std::string out;
    for (const char c : id) {
        if (c == ':') {
            if (out.empty() || out.back() != '_') {
                out += '_';
            }
            continue;
        }
        out += c;
    }
    return out;
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

// A node's paint text: a string literal, or an all-text template folded by codegen. Dynamic text
// needs the string runtime and is rejected by the caller.
std::optional<std::string> text_content(const ir::Expr* expr) {
    if (const std::optional<std::string_view> literal = literal_string(expr)) {
        return std::string(*literal);
    }
    if (expr == nullptr || expr->kind != ir::ExprKind::Template) {
        return std::nullopt;
    }
    std::string folded;
    for (const ir::TemplatePart& part : expr->parts) {
        if (part.expr) {
            return std::nullopt;
        }
        folded += part.text;
    }
    return folded;
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
               unsigned& next_id, unsigned& next_var, std::string& variable, std::string& error) {
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
    if (node.text && !text_content(node.text.get())) {
        return fail(error, "text: dynamic text needs the text runtime, deferred");
    }
    const std::string id = node_id_text(node, next_id, error);
    if (!error.empty()) {
        return false;
    }
    writer.line(node_comment(node));
    variable = "node_" + std::to_string(next_var++);
    writer.line("auto " + variable + " = std::make_unique<tessera::ui::Node>(" + id + ");");
    // Deterministic metadata order: element kind, id, className, automationId, role, aria-* (name
    // order), focusability/scroll flags, paint text, then the child subtrees.
    const auto prop = [&](std::string_view name) -> const ir::Expr* {
        const auto found = node.props.find(std::string(name));
        return found == node.props.end() ? nullptr : found->second.get();
    };
    writer.line(variable + "->set_element_kind(\"" + std::string(ir::node_kind_name(node.kind)) +
                "\");");
    if (const ir::Expr* id_prop = prop("id")) {
        const std::optional<std::string_view> text = literal_string(id_prop);
        if (!text) {
            return fail(error, "id: a dynamic element id needs the string runtime, deferred");
        }
        writer.line(variable + "->set_element_id(" +
                    literal_text(ir::LiteralValue{std::string(*text)}) + ");");
    }
    if (const ir::Expr* class_prop = prop("className")) {
        const std::optional<std::string_view> text = literal_string(class_prop);
        if (!text) {
            return fail(error, "className: a dynamic class needs the string runtime, deferred");
        }
        writer.line(variable + "->set_class_name(" +
                    literal_text(ir::LiteralValue{std::string(*text)}) + ");");
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
        if (!text) {
            return fail(error, "role: a dynamic role needs the string runtime, deferred");
        }
        writer.line(variable + "->set_role(" + literal_text(ir::LiteralValue{std::string(*text)}) +
                    ");");
    }
    for (const auto& [name, value] : node.props) {
        if (name.rfind("aria-", 0) != 0) {
            continue;
        }
        const std::optional<std::string_view> text = literal_string(value.get());
        if (!text) {
            return fail(error, name + ": a dynamic aria value needs the string runtime, deferred");
        }
        writer.line(variable + "->set_aria(" + literal_text(ir::LiteralValue{std::string(name)}) +
                    ", " + literal_text(ir::LiteralValue{std::string(*text)}) + ");");
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
    if (const std::optional<std::string> paint_text = text_content(node.text.get())) {
        // D9 paint payload: text is visual only (layout/wrap stay paint-side).
        const std::string paint = "paint_" + variable;
        writer.line("tessera::ui::PaintData " + paint + ";");
        writer.line(paint + ".text = " + literal_text(ir::LiteralValue{std::string(*paint_text)}) +
                    ";");
        writer.line(variable + "->set_paint_data(std::move(" + paint + "));");
    }
    for (const ir::Node& child : node.children) {
        std::string child_variable;
        if (!emit_node(writer, child, component, next_id, next_var, child_variable, error)) {
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

bool emit_header(const ir::Component& component, std::string_view ns,
                 const std::vector<const ir::Binding*>& bindings, GeneratedFile& file,
                 std::string& error) {
    CppWriter writer;
    writer.line("// SPDX-License-Identifier: MIT");
    writer.line("// Generated by Tessera codegen from the IR; see docs/design/codegen.md.");
    writer.line("// Developer code belongs only inside [DEVELOPER_LOGIC_START/END] regions.");
    writer.line("#pragma once");
    writer.line("");
    writer.line("#include \"tessera/ui/scene/node.hpp\"");
    writer.line("");
    writer.line("#include <cstdint>");
    writer.line("#include <memory>");
    writer.line("#include <optional>");
    writer.line("#include <string>");
    writer.line("#include <string_view>");
    writer.line("#include <vector>");
    writer.line("");
    writer.open("namespace " + std::string(ns));
    writer.open("namespace gen");
    writer.line("// " + component.id + " (" + component.name + ").");
    writer.open("struct " + component.name);
    for (const ir::State& state : component.state) {
        const std::optional<std::string> type = cpp_type(state.type);
        if (!type) {
            return fail(error, "state `" + state.name + "` has a type with no C++ form (`" +
                                   state.type + "`)");
        }
        ExprEmission initial =
            state.initial ? emit_expr(*state.initial, component) : ExprEmission{"{}", {}};
        if (!initial.error.empty()) {
            return fail(error, initial.error);
        }
        writer.line("// useState `" + state.name + "`.");
        writer.line(*type + " " + member_name(state.id) + " = " + initial.text + ";");
    }
    for (const ir::Ref& ref : component.refs) {
        writer.line("// useRef `" + ref.name + "` (filled by the ref-target assignment slice).");
        writer.line("tessera::ui::Node* " + member_name(ref.id) + " = nullptr;");
    }
    for (const ir::AnimatedValue& value : component.animated_values) {
        const std::optional<std::string> type = cpp_type(value.type);
        if (!type) {
            return fail(error, "animated value `" + value.name + "` has a type with no C++ form");
        }
        ExprEmission initial =
            value.initial ? emit_expr(*value.initial, component) : ExprEmission{"{}", {}};
        if (!initial.error.empty()) {
            return fail(error, initial.error);
        }
        writer.line("// useAnimatedValue `" + value.name +
                    "` (a plain field until the animation controller API exists).");
        writer.line(*type + " " + member_name(value.id) + " = " + initial.text + ";");
    }
    for (const ir::Prop& prop : component.props) {
        const std::optional<std::string> type = cpp_type(prop.type);
        if (!type) {
            return fail(error, "prop `" + prop.name + "` has a type with no C++ form (`" +
                                   prop.type + "`)");
        }
        std::string initial = "{}";
        if (prop.default_value) {
            ExprEmission value = emit_expr(*prop.default_value, component);
            if (!value.error.empty()) {
                return fail(error, value.error);
            }
            initial = "{" + value.text + "}";
        }
        writer.line("// prop `" + prop.name + "`.");
        writer.line(*type + " " + member_name("prop::" + prop.name) + " = " + initial + ";");
    }
    writer.line("");
    writer.line("void build(tessera::ui::Node& parent);");
    for (const ir::Handler& handler : component.handlers) {
        writer.line("void " + method_name(handler.id) + "(" + std::string(kEventType) + " event);");
    }
    for (const ir::Binding* binding : bindings) {
        if (binding->kind == ir::BindingKind::Event) {
            writer.line("void " + binding->region + "(" + params_text(binding->params) + ");");
            continue;
        }
        std::string return_type;
        if (!binding->return_type.empty()) {
            return_type = handler_type(binding->return_type);
            if (return_type.empty()) {
                return fail(error, "binding `" + binding->id +
                                       "` has an unsupported return type `" + binding->return_type +
                                       "`");
            }
        }
        writer.line(std::string(return_type.empty() ? "void" : return_type) + " " +
                    binding->region + "();");
    }
    writer.close(";");
    writer.close("");
    writer.close("");
    file.path = component.name + ".hpp";
    file.text = std::move(writer.text);
    return true;
}

bool emit_stub_body(CppWriter& writer, const ir::Binding& binding, bool event_parameter) {
    writer.line("// TODO(tessera-codegen): implement the developer body for " + binding.id + ".");
    if (event_parameter) {
        writer.line("(void)event;");
    }
    if (!binding.return_type.empty()) {
        writer.line("return {};");
    }
    return true;
}

bool emit_source(const ir::Component& component, std::string_view ns,
                 const std::vector<const ir::Binding*>& bindings, GeneratedFile& file,
                 std::string& error) {
    CppWriter writer;
    writer.line("// SPDX-License-Identifier: MIT");
    writer.line("// Generated by Tessera codegen from the IR; see docs/design/codegen.md.");
    writer.line("// Developer code belongs only inside [DEVELOPER_LOGIC_START/END] regions.");
    writer.line("#include \"" + component.name + ".hpp\"");
    writer.line("");
    writer.line("#include <memory>");
    writer.line("#include <utility>");
    writer.line("");
    writer.open("namespace " + std::string(ns));
    writer.open("namespace gen");
    writer.line(
        "// TODO(tessera-codegen): remaining runtime API gaps are inline style attachment, event");
    writer.line(
        "// payload records, event dispatch wiring, ref-target assignment and animated/effect");
    writer.line(
        "// lifecycle invocation (docs/design/compiler.md section 11). Style and events are");
    writer.line("// recorded in the node comments below until those land.");
    writer.line("");
    writer.open("void " + component.name + "::build(tessera::ui::Node& parent)");
    unsigned next_id = 1;
    unsigned next_var = 1;
    std::string root_variable;
    if (!emit_node(writer, component.root, component, next_id, next_var, root_variable, error)) {
        return false;
    }
    writer.line("parent.add_child(std::move(" + root_variable + "));");
    writer.close("");
    writer.line("");
    for (const ir::Handler& handler : component.handlers) {
        const std::string name = method_name(handler.id);
        writer.open("void " + component.name + "::" + name + "(" + std::string(kEventType) +
                    " event)");
        writer.line("(void)event;");
        std::string action_error;
        const std::optional<std::string> body =
            emit_actions(handler.actions, component, action_error);
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
        writer.close("");
        writer.line("");
    }
    for (const ir::Binding* binding : bindings) {
        const bool event_parameter = binding->kind == ir::BindingKind::Event;
        std::string signature;
        if (event_parameter) {
            signature = "void " + component.name + "::" + binding->region + "(" +
                        params_text(binding->params) + ")";
        } else if (!binding->return_type.empty()) {
            const std::string type = handler_type(binding->return_type);
            signature = type + " " + component.name + "::" + binding->region + "()";
        } else {
            signature = "void " + component.name + "::" + binding->region + "()";
        }
        writer.open(signature);
        writer.line("// [DEVELOPER_LOGIC_START: " + binding->id + "]");
        writer.indent += 1;
        emit_stub_body(writer, *binding, event_parameter);
        writer.indent -= 1;
        writer.line("// [DEVELOPER_LOGIC_END: " + binding->id + "]");
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
    GeneratedFile header;
    if (!emit_header(component, module_namespace, owned, header, error)) {
        return false;
    }
    GeneratedFile source;
    if (!emit_source(component, module_namespace, owned, source, error)) {
        return false;
    }
    files.push_back(std::move(header));
    files.push_back(std::move(source));
    return true;
}

} // namespace tessera::compiler::codegen::internal
