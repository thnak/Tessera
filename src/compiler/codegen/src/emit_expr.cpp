// SPDX-License-Identifier: MIT

// IR expressions -> C++ source text (docs/design/codegen.md "Expression emission").
//
// Every kind that has no correct C++ spelling yet is rejected with TESSERA_CODEGEN_UNSUPPORTED
// instead of emitting code that does not compile or silently changes semantics. The type table
// (compiler slice 4) is what will lift the deferred kinds (`%`, mixed record fields, text
// templates with substitutions).

#include "codegen_internal.hpp"

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace tessera::compiler::codegen::internal {
namespace {

constexpr std::string_view kState = "state::";
constexpr std::string_view kProp = "prop::";
constexpr std::string_view kRef = "ref::";
constexpr std::string_view kAnim = "anim::";
constexpr std::string_view kEnv = "env::";

std::string escape_string(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                constexpr char kHex[] = "0123456789abcdef";
                out += "\\u00";
                out += kHex[(static_cast<unsigned char>(c) >> 4) & 0xF];
                out += kHex[static_cast<unsigned char>(c) & 0xF];
            } else {
                out += c;
            }
            break;
        }
    }
    out += '"';
    return out;
}

std::string number_text(double value) {
    char buffer[64];
    const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof buffer, value);
    std::string out(buffer, result.ptr);
    if (out.find_first_of(".eE") == std::string::npos) {
        out += ".0";
    }
    return out;
}

ExprEmission fail(std::string message) {
    return ExprEmission{"", std::move(message)};
}

std::optional<std::string> member_prefix(std::string_view prefix) {
    if (prefix == kState) {
        return std::string("state_");
    }
    if (prefix == kProp) {
        return std::string("prop_");
    }
    if (prefix == kRef) {
        return std::string("ref_");
    }
    if (prefix == kAnim) {
        return std::string("anim_");
    }
    if (prefix == kEnv) {
        return std::string("env_");
    }
    return std::nullopt;
}

} // namespace

std::string literal_text(const ir::LiteralValue& literal) {
    if (const auto* flag = std::get_if<bool>(&literal)) {
        return *flag ? "true" : "false";
    }
    if (const auto* integer = std::get_if<std::int64_t>(&literal)) {
        return std::to_string(*integer);
    }
    if (const auto* number = std::get_if<double>(&literal)) {
        return number_text(*number);
    }
    if (const auto* text = std::get_if<std::string>(&literal)) {
        return escape_string(*text);
    }
    return "nullptr";
}

std::string animated_controller_name(std::string_view ir_id) {
    std::string name = member_name(ir_id);
    if (!name.empty() && name.back() == '_') {
        name.pop_back();
    }
    return name + "_scalar_";
}

std::string animated_on_complete_name(std::string_view ir_id) {
    std::string name = member_name(ir_id);
    if (!name.empty() && name.back() == '_') {
        name.pop_back();
    }
    return name + "_on_complete_";
}

bool animated_has_completion(const ir::Component& component, std::string_view ir_id) {
    for (const ir::Handler& handler : component.handlers) {
        for (const ir::Action& action : handler.actions) {
            if (action.kind == ir::ActionKind::AnimSet && action.target == ir_id &&
                !action.on_complete.empty()) {
                return true;
            }
        }
    }
    return false;
}

std::string member_name(std::string_view ir_id) {
    const std::size_t split = ir_id.find("::");
    if (split == std::string_view::npos) {
        return std::string(ir_id) + "_";
    }
    const std::string_view prefix = ir_id.substr(0, split);
    const std::string_view name = ir_id.substr(split + 2);
    const std::optional<std::string> mapped = member_prefix(prefix);
    return (mapped ? *mapped : std::string(prefix) + "_") + std::string(name) + "_";
}

std::optional<std::string> cpp_type(std::string_view ir_type) {
    if (ir_type == "bool") {
        return std::string("bool");
    }
    if (ir_type == "int") {
        return std::string("int");
    }
    if (ir_type == "float") {
        return std::string("double");
    }
    if (ir_type == "string" || ir_type == "color") {
        return std::string("std::string");
    }
    if (ir_type == "length") {
        return std::string("double");
    }
    if (ir_type == "node_ref") {
        return std::string("tessera::ui::Node*");
    }
    for (const std::string_view wrapper : {"list<", "optional<"}) {
        if (ir_type.size() > wrapper.size() + 1 && ir_type.rfind(wrapper, 0) == 0 &&
            ir_type.back() == '>') {
            const std::optional<std::string> inner =
                cpp_type(ir_type.substr(wrapper.size(), ir_type.size() - wrapper.size() - 1));
            if (!inner) {
                return std::nullopt;
            }
            if (wrapper == "list<") {
                return "std::vector<" + *inner + ">";
            }
            return "std::optional<" + *inner + ">";
        }
    }
    // A record type name has no generated C++ struct in this slice.
    return std::nullopt;
}

const LocalBinding* find_local(const LocalBindings& locals, std::string_view id) {
    for (const LocalBinding& local : locals) {
        if (local.name == id) {
            return &local;
        }
    }
    return nullptr;
}

ExprEmission emit_expr(const ir::Expr& expr, const ir::Component& component,
                       const LocalBindings& locals) {
    (void)component;
    switch (expr.kind) {
    case ir::ExprKind::Literal:
        return ExprEmission{literal_text(expr.literal), {}};
    case ir::ExprKind::Ref: {
        if (const LocalBinding* local = find_local(locals, expr.id)) {
            return ExprEmission{local->cpp, {}};
        }
        if (const std::size_t dot = expr.id.find('.'); dot != std::string::npos) {
            if (find_local(locals, std::string_view(expr.id).substr(0, dot)) != nullptr) {
                return fail("`" + expr.id +
                            "` reads a field of a loop item; record element types have no C++ form "
                            "in this codegen slice (list element types are primitives only)");
            }
            return fail("record field `" + expr.id +
                        "` needs the type table (slice 4); unsupported in this codegen slice");
        }
        const std::size_t split = expr.id.find("::");
        if (split == std::string::npos || !member_prefix(expr.id.substr(0, split + 2))) {
            return fail("unsupported reference `" + expr.id +
                        "`; a local is only visible inside the list template that binds it");
        }
        return ExprEmission{member_name(expr.id), {}};
    }
    case ir::ExprKind::Binary: {
        if (expr.op == "%") {
            return fail("`%` needs the type table (int vs float remainder), deferred");
        }
        if (expr.operands.size() != 2 || !expr.operands[0] || !expr.operands[1]) {
            return fail("a binary expression needs two operands");
        }
        if (expr.op == "==" || expr.op == "!=") {
            // `e.key == "Escape"` (issue #236): the C++ payload carries a key code, not the W3C
            // string, so the comparison goes through `key_is`.
            for (std::size_t side = 0; side < 2; ++side) {
                const ir::Expr& key = *expr.operands[side];
                const ir::Expr& name = *expr.operands[1 - side];
                const LocalBinding* local =
                    key.kind == ir::ExprKind::Ref ? find_local(locals, key.id) : nullptr;
                if (local == nullptr || local->ir_type != "KeyName") {
                    continue;
                }
                const auto* text = name.kind == ir::ExprKind::Literal
                                       ? std::get_if<std::string>(&name.literal)
                                       : nullptr;
                if (text == nullptr) {
                    return fail("`" + key.id + "` can only be compared with a string literal");
                }
                return ExprEmission{std::string(expr.op == "!=" ? "(!" : "(") +
                                        "tessera::ui::key_is(" + local->cpp + ", " +
                                        escape_string(*text) + "))",
                                    {}};
            }
        }
        ExprEmission left = emit_expr(*expr.operands[0], component, locals);
        if (!left.error.empty()) {
            return left;
        }
        ExprEmission right = emit_expr(*expr.operands[1], component, locals);
        if (!right.error.empty()) {
            return right;
        }
        if (expr.op == "/") {
            // IR `/` always yields float (ir-v1 section 4.1); C++ integer division would truncate.
            return ExprEmission{"(static_cast<double>(" + left.text + ") / static_cast<double>(" +
                                    right.text + "))",
                                {}};
        }
        return ExprEmission{"(" + left.text + " " + expr.op + " " + right.text + ")", {}};
    }
    case ir::ExprKind::Unary: {
        if (expr.operands.size() != 1 || !expr.operands[0]) {
            return fail("a unary expression needs one operand");
        }
        ExprEmission value = emit_expr(*expr.operands[0], component, locals);
        if (!value.error.empty()) {
            return value;
        }
        return ExprEmission{"(" + expr.op + value.text + ")", {}};
    }
    case ir::ExprKind::Conditional: {
        if (expr.operands.size() != 3 || !expr.operands[0] || !expr.operands[1] ||
            !expr.operands[2]) {
            return fail("a conditional needs three operands");
        }
        ExprEmission test = emit_expr(*expr.operands[0], component, locals);
        ExprEmission then_value = emit_expr(*expr.operands[1], component, locals);
        ExprEmission else_value = emit_expr(*expr.operands[2], component, locals);
        if (!test.error.empty()) {
            return test;
        }
        if (!then_value.error.empty()) {
            return then_value;
        }
        if (!else_value.error.empty()) {
            return else_value;
        }
        return ExprEmission{
            "(" + test.text + " ? " + then_value.text + " : " + else_value.text + ")", {}};
    }
    case ir::ExprKind::Template: {
        std::string folded;
        for (const ir::TemplatePart& part : expr.parts) {
            if (part.expr) {
                return fail("a text template with substitutions needs the text runtime, deferred");
            }
            folded += part.text;
        }
        return ExprEmission{escape_string(folded), {}};
    }
    case ir::ExprKind::Call: {
        static constexpr std::pair<std::string_view, std::string_view> kCalls[] = {
            {"min", "std::min"},     {"max", "std::max"},     {"clamp", "std::clamp"},
            {"abs", "std::abs"},     {"floor", "std::floor"}, {"ceil", "std::ceil"},
            {"round", "std::round"},
        };
        if (expr.id == "length" || expr.id == "formatNumber") {
            return fail("`" + expr.id + "` needs the string/format runtime, deferred");
        }
        if (expr.id == "toInt" || expr.id == "toFloat") {
            if (expr.operands.size() != 1 || !expr.operands[0]) {
                return fail("`" + expr.id + "` takes one argument");
            }
            ExprEmission value = emit_expr(*expr.operands[0], component, locals);
            if (!value.error.empty()) {
                return value;
            }
            return ExprEmission{
                (expr.id == "toInt" ? "static_cast<int>(" : "static_cast<double>(") + value.text +
                    ")",
                {}};
        }
        std::string_view callee;
        for (const auto& [name, cpp] : kCalls) {
            if (expr.id == name) {
                callee = cpp;
                break;
            }
        }
        if (callee.empty()) {
            return fail("unsupported function `" + expr.id + "`; no C++ spelling in this slice");
        }
        std::string text = std::string(callee) + "(";
        for (std::size_t i = 0; i < expr.operands.size(); ++i) {
            if (!expr.operands[i]) {
                return fail("a call argument is missing");
            }
            ExprEmission arg = emit_expr(*expr.operands[i], component, locals);
            if (!arg.error.empty()) {
                return arg;
            }
            if (i != 0) {
                text += ", ";
            }
            text += arg.text;
        }
        text += ")";
        return ExprEmission{std::move(text), {}};
    }
    case ir::ExprKind::Array:
        // An array literal is only a state/prop initial value (docs/specs/ir-v1.md section 4); the
        // initializer position has its own emitter so a braced list never leaks into an expression.
        return fail("an array literal has no C++ spelling in an expression position; it is only a "
                    "state or prop initial value");
    }
    return fail("unsupported expression kind");
}

ExprEmission emit_initializer_expr(const ir::Expr& expr, const ir::Component& component) {
    if (expr.kind != ir::ExprKind::Array) {
        return emit_expr(expr, component);
    }
    std::string text = "{";
    for (std::size_t i = 0; i < expr.operands.size(); ++i) {
        if (!expr.operands[i]) {
            return fail("an array initial has a missing element");
        }
        const ExprEmission element = emit_expr(*expr.operands[i], component);
        if (!element.error.empty()) {
            return element;
        }
        if (i != 0) {
            text += ", ";
        }
        text += element.text;
    }
    text += "}";
    return ExprEmission{std::move(text), {}};
}

// `e.width`, `e.contentHeight`, ...: the fields of a SizeChangeEvent parameter, spelled the way the
// C++ record names them, so an expression like `e.width >= 600` has a C++ form.
namespace {
LocalBindings payload_field_locals(std::span<const ir::Param> params) {
    LocalBindings locals;
    for (const ir::Param& param : params) {
        if (param.type == "KeyEvent" && !param.name.empty()) {
            locals.push_back({param.name + ".key", param.name, "KeyName"});
            continue;
        }
        if (param.type != "SizeChangeEvent" || param.name.empty()) {
            continue;
        }
        locals.push_back({param.name + ".width", param.name + ".width", "float"});
        locals.push_back({param.name + ".height", param.name + ".height", "float"});
        locals.push_back({param.name + ".contentWidth", param.name + ".content_width", "float"});
        locals.push_back({param.name + ".contentHeight", param.name + ".content_height", "float"});
    }
    return locals;
}
// One action as C++ statements appended to `body`; false with `error` set on failure.
bool emit_one_action(const ir::Action& action, const ir::Component& component,
                     std::span<const ir::Param> params, std::string& body, std::string& error) {
    if (action.kind == ir::ActionKind::Focus || action.kind == ir::ActionKind::Blur) {
        // The component reaches the scene's FocusManager through `set_focus_manager`; before
        // the host provides one (or before build() assigns the ref) the call is a no-op.
        const std::string node = member_name(action.target);
        body += "if (focus_manager_ != nullptr && " + node + " != nullptr) {\n";
        if (action.kind == ir::ActionKind::Focus) {
            body += "focus_manager_->request_focus(" + node + ");\n";
        } else {
            body += "if (focus_manager_->focused() == " + node +
                    ") {\nfocus_manager_->clear_focus();\n}\n";
        }
        body += "}\n";
        return true;
    }
    if (!action.value) {
        error = "an action has no value expression";
        return false;
    }
    ExprEmission value;
    const ir::Expr& value_expr = *action.value;
    const std::size_t dot = value_expr.id.find('.');
    if (value_expr.kind == ir::ExprKind::Ref && dot != std::string::npos) {
        const std::string_view base = std::string_view(value_expr.id).substr(0, dot);
        const std::string_view field = std::string_view(value_expr.id).substr(dot + 1);
        for (const ir::Param& param : params) {
            if (field == "value" && param.name == base && param.type == "TextChangeEvent") {
                value.text = "std::string(" + std::string(base) + ".value)";
                break;
            }
        }
    }
    if (value.text.empty()) {
        value = emit_expr(*action.value, component, payload_field_locals(params));
    }
    if (!value.error.empty()) {
        error = value.error;
        return false;
    }
    const std::string_view target = action.target;
    if (action.kind == ir::ActionKind::Set && target.rfind(kState, 0) == 0) {
        body += member_name(target) + " = " + value.text + ";\n";
        return true;
    }
    if (action.kind == ir::ActionKind::AnimSet && target.rfind(kAnim, 0) == 0) {
        // The controller moves the sampled field toward the value over the frame ticks; a snap
        // (`animate: false`, a zero duration) lands in the field at once. Timing is literal in
        // the IR, so the easing is parsed once per call site.
        const std::string controller = animated_controller_name(target);
        body += "{\n";
        if (animated_has_completion(component, target)) {
            // Which handler runs when THIS set completes; a set without one clears it, so a
            // superseded animation never fires the old callback.
            std::size_t handler_index = 0;
            for (std::size_t i = 0; i < component.handlers.size(); ++i) {
                if (!action.on_complete.empty() && component.handlers[i].id == action.on_complete) {
                    handler_index = i + 1;
                }
            }
            if (!action.on_complete.empty() && handler_index == 0) {
                error = "onComplete handler `" + action.on_complete + "` is not declared";
                return false;
            }
            body +=
                animated_on_complete_name(target) + " = " + std::to_string(handler_index) + ";\n";
        }
        if (!action.animate) {
            body += "tessera::ui::animation::ScalarOptions options;\noptions.animate = false;\n";
        } else if (action.spring.has_value()) {
            const ir::SpringTiming& spring = *action.spring;
            body += "tessera::ui::animation::ScalarOptions options;\n"
                    "options.spring = tessera::ui::animation::SpringConfig{};\n"
                    "options.spring->stiffness = static_cast<float>(" +
                    literal_text(spring.stiffness) +
                    ");\n"
                    "options.spring->damping = static_cast<float>(" +
                    literal_text(spring.damping) +
                    ");\n"
                    "options.spring->mass = static_cast<float>(" +
                    literal_text(spring.mass) + ");\n";
        } else if (action.duration_ms.has_value() || !action.easing.empty()) {
            body += "tessera::ui::animation::ScalarOptions options;\n";
            if (action.duration_ms.has_value()) {
                body +=
                    "options.duration_s = " + literal_text(*action.duration_ms / 1000.0) + ";\n";
            }
            if (!action.easing.empty()) {
                body += "static const tessera::ui::animation::Easing easing =\n"
                        "tessera::ui::animation::parse_easing(\"" +
                        action.easing + "\").easing;\noptions.easing = easing;\n";
            }
        } else {
            body += "const tessera::ui::animation::ScalarOptions options;\n";
        }
        body += controller + ".set(static_cast<double>(" + value.text + "), options);\n" +
                member_name(target) + " = " + controller + ".value();\n" + "if (" + controller +
                ".active() || " + controller +
                ".completion_pending()) {\nschedule_update();\n}\n}\n";
        return true;
    }
    error = "unsupported action target `" + action.target + "`";
    return false;
}

} // namespace

std::optional<std::string> emit_actions(const std::vector<ir::Action>& actions,
                                        const ir::Component& component,
                                        std::span<const ir::Param> params, std::string& error) {
    std::string body;
    for (const ir::Action& action : actions) {
        std::string one;
        if (!emit_one_action(action, component, params, one, error)) {
            return std::nullopt;
        }
        if (!action.guard) {
            body += one;
            continue;
        }
        // A guarded statement (`e.key === "Escape" && setOpen(false)`, issue #236) runs only when
        // its pure guard holds.
        const ExprEmission guard =
            emit_expr(*action.guard, component, payload_field_locals(params));
        if (!guard.error.empty()) {
            error = guard.error;
            return std::nullopt;
        }
        body += "if (" + guard.text + ") {\n" + one + "}\n";
    }
    return body;
}

} // namespace tessera::compiler::codegen::internal
