// SPDX-License-Identifier: MIT

// Expression and string-literal encoding for the build-time library component IR writer (issue
// #70). The output is C++ source that calls the `ir::make_*` constructors; it is a pure function of
// the input expression.

#include "emit_library_components_internal.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace tessera::compiler::codegen::cli::internal {
namespace {

namespace ir = tessera::compiler::ir;

std::optional<std::string> literal_source(const ir::Expr& expr, std::string& error) {
    if (const auto* text = std::get_if<std::string>(&expr.literal)) {
        return "ir::make_literal(std::string(" + quoted(*text) + "))";
    }
    if (const auto* flag = std::get_if<bool>(&expr.literal)) {
        return std::string("ir::make_literal(") + (*flag ? "true" : "false") + ")";
    }
    if (const auto* whole = std::get_if<std::int64_t>(&expr.literal)) {
        char buffer[32];
        const auto result = std::to_chars(buffer, buffer + sizeof buffer, *whole);
        return "ir::make_literal(std::int64_t{" + std::string(buffer, result.ptr) + "})";
    }
    if (const auto* real = std::get_if<double>(&expr.literal)) {
        if (!std::isfinite(*real)) {
            error = "a float literal is not finite";
            return std::nullopt;
        }
        char buffer[64];
        const auto result = std::to_chars(buffer, buffer + sizeof buffer, *real);
        std::string text(buffer, result.ptr);
        if (text.find_first_of(".eE") == std::string::npos) {
            text += ".0"; // keep the literal a double, never an int
        }
        return "ir::make_literal(" + text + ")";
    }
    if (std::holds_alternative<ir::Null>(expr.literal)) {
        return "ir::make_literal(ir::Null{})";
    }
    error = "the literal value has no C++ spelling";
    return std::nullopt;
}

bool expr_list(const std::vector<ir::ExprPtr>& operands, std::string& out, std::string& error) {
    if (operands.empty()) {
        out = "[] { return std::vector<ir::ExprPtr>(); }()";
        return true;
    }
    std::string body;
    for (const ir::ExprPtr& operand : operands) {
        if (!operand) {
            error = "an expression operand is missing";
            return false;
        }
        std::string item;
        if (!expr_source(*operand, item, error)) {
            return false;
        }
        body += "items.push_back(" + item + "); ";
    }
    out = "[] { std::vector<ir::ExprPtr> items; " + body + "return items; }()";
    return true;
}

} // namespace

std::string escape_string(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char raw : text) {
        const auto byte = static_cast<unsigned char>(raw);
        switch (byte) {
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
            if (byte < 0x20 || byte == 0x7f) {
                out += '\\';
                out += static_cast<char>('0' + ((byte >> 6) & 0x7));
                out += static_cast<char>('0' + ((byte >> 3) & 0x7));
                out += static_cast<char>('0' + (byte & 0x7));
            } else {
                out += static_cast<char>(byte);
            }
        }
    }
    return out;
}

std::string quoted(std::string_view text) {
    return "\"" + escape_string(text) + "\"";
}

bool is_project_relative(std::string_view file) {
    if (file.empty()) {
        return false;
    }
    const bool absolute = file.front() == '/' || (file.size() > 1 && file[1] == ':');
    const bool escapes = file == ".." || file.starts_with("../") ||
                         file.find("/../") != std::string_view::npos || file.ends_with("/..");
    return !absolute && !escapes;
}

bool expr_source(const ir::Expr& expr, std::string& out, std::string& error) {
    switch (expr.kind) {
    case ir::ExprKind::Literal: {
        const std::optional<std::string> literal = literal_source(expr, error);
        if (!literal) {
            return false;
        }
        out = *literal;
        return true;
    }
    case ir::ExprKind::Ref:
        out = "ir::make_ref(" + quoted(expr.id) + ")";
        return true;
    case ir::ExprKind::Binary:
        if (expr.operands.size() != 2) {
            error = "a binary expression needs two operands";
            return false;
        }
        break;
    case ir::ExprKind::Unary:
        if (expr.operands.size() != 1) {
            error = "a unary expression needs one operand";
            return false;
        }
        break;
    case ir::ExprKind::Conditional:
        if (expr.operands.size() != 3) {
            error = "a conditional expression needs three operands";
            return false;
        }
        break;
    case ir::ExprKind::Call:
        break;
    case ir::ExprKind::Template: {
        if (expr.parts.empty()) {
            out = "ir::make_template(std::vector<ir::TemplatePart>())";
            return true;
        }
        std::string body;
        for (const ir::TemplatePart& part : expr.parts) {
            std::string piece;
            if (part.expr) {
                std::string inner;
                if (!expr_source(*part.expr, inner, error)) {
                    return false;
                }
                piece = "ir::expr_part(" + inner + ")";
            } else {
                piece = "ir::text_part(" + quoted(part.text) + ")";
            }
            body += "parts.push_back(" + piece + "); ";
        }
        out = "[] { std::vector<ir::TemplatePart> parts; " + body +
              "return ir::make_template(std::move(parts)); }()";
        return true;
    }
    case ir::ExprKind::Array:
        if (!expr_list(expr.operands, out, error)) {
            return false;
        }
        out = "ir::make_array(" + out + ")";
        return true;
    }
    std::string operands;
    if (!expr_list(expr.operands, operands, error)) {
        return false;
    }
    switch (expr.kind) {
    case ir::ExprKind::Binary:
        out = "ir::make_binary(" + quoted(expr.op) + ", " + operands + ")";
        return true;
    case ir::ExprKind::Unary:
        out = "ir::make_unary(" + quoted(expr.op) + ", " + operands + ")";
        return true;
    case ir::ExprKind::Conditional:
        out = "ir::make_conditional(" + operands + ")";
        return true;
    case ir::ExprKind::Call:
        out = "ir::make_call(" + quoted(expr.id) + ", " + operands + ")";
        return true;
    default:
        break;
    }
    error = "the expression kind has no generated C++ form";
    return false;
}

} // namespace tessera::compiler::codegen::cli::internal
