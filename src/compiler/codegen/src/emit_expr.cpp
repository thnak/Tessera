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

ExprEmission emit_expr(const ir::Expr& expr, const ir::Component& component) {
    (void)component;
    switch (expr.kind) {
    case ir::ExprKind::Literal:
        return ExprEmission{literal_text(expr.literal), {}};
    case ir::ExprKind::Ref: {
        if (expr.id.find('.') != std::string::npos) {
            return fail("record field `" + expr.id +
                        "` needs the type table (slice 4); unsupported in this codegen slice");
        }
        const std::size_t split = expr.id.find("::");
        if (split == std::string::npos || !member_prefix(expr.id.substr(0, split + 2))) {
            return fail("unsupported reference `" + expr.id + "`");
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
        ExprEmission left = emit_expr(*expr.operands[0], component);
        if (!left.error.empty()) {
            return left;
        }
        ExprEmission right = emit_expr(*expr.operands[1], component);
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
        ExprEmission value = emit_expr(*expr.operands[0], component);
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
        ExprEmission test = emit_expr(*expr.operands[0], component);
        ExprEmission then_value = emit_expr(*expr.operands[1], component);
        ExprEmission else_value = emit_expr(*expr.operands[2], component);
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
            ExprEmission value = emit_expr(*expr.operands[0], component);
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
            ExprEmission arg = emit_expr(*expr.operands[i], component);
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
    }
    return fail("unsupported expression kind");
}

std::optional<std::string> emit_actions(const std::vector<ir::Action>& actions,
                                        const ir::Component& component,
                                        std::span<const ir::Param> params, std::string& error) {
    std::string body;
    for (const ir::Action& action : actions) {
        if (!action.value) {
            error = "an action has no value expression";
            return std::nullopt;
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
            value = emit_expr(*action.value, component);
        }
        if (!value.error.empty()) {
            error = value.error;
            return std::nullopt;
        }
        const std::string_view target = action.target;
        if (action.kind == ir::ActionKind::Set && target.rfind(kState, 0) == 0) {
            body += member_name(target) + " = " + value.text + ";\n";
            continue;
        }
        if (action.kind == ir::ActionKind::AnimSet && target.rfind(kAnim, 0) == 0) {
            body += member_name(target) + " = " + value.text +
                    "; // TODO(tessera-codegen): animate through the animation controller API\n";
            continue;
        }
        error = "unsupported action target `" + action.target + "`";
        return std::nullopt;
    }
    return body;
}

} // namespace tessera::compiler::codegen::internal
