// SPDX-License-Identifier: MIT

#include "tessera/compiler/jsx/parser.hpp"

namespace tessera::compiler::jsx {
namespace {

void append_quoted(std::string& out, const std::string& text) {
    out.push_back('"');
    for (const char c : text) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
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
            out.push_back(c);
            break;
        }
    }
    out.push_back('"');
}

void dump_expr(std::string& out, const Expr& expr);

void dump_operands(std::string& out, const std::vector<ExprPtr>& operands, std::size_t first = 0) {
    for (std::size_t i = first; i < operands.size(); ++i) {
        out.push_back(' ');
        dump_expr(out, *operands[i]);
    }
}

void dump_jsx(std::string& out, const JsxNode& node) {
    switch (node.kind) {
    case JsxKind::Text:
        out += "(text ";
        append_quoted(out, node.text);
        out.push_back(')');
        return;
    case JsxKind::ExprContainer:
        out += "(expr";
        if (node.expr) {
            out.push_back(' ');
            dump_expr(out, *node.expr);
        }
        out.push_back(')');
        return;
    case JsxKind::Fragment:
        out += "(fragment";
        break;
    case JsxKind::Element:
        out += "(element " + node.tag + (node.self_closing ? "/" : "");
        for (const auto& attribute : node.attributes) {
            switch (attribute.kind) {
            case JsxAttrKind::Flag:
                out += " (flag " + attribute.name + ")";
                break;
            case JsxAttrKind::String:
                out += " (attr " + attribute.name + " ";
                append_quoted(out, attribute.string_value);
                out.push_back(')');
                break;
            case JsxAttrKind::Expression:
                out += " (attr " + attribute.name + " ";
                dump_expr(out, *attribute.expr);
                out.push_back(')');
                break;
            case JsxAttrKind::Spread:
                out += " (spread ";
                dump_expr(out, *attribute.expr);
                out.push_back(')');
                break;
            }
        }
        break;
    }
    for (const auto& child : node.children) {
        out.push_back(' ');
        dump_jsx(out, *child);
    }
    out.push_back(')');
}

void dump_expr(std::string& out, const Expr& expr) {
    switch (expr.kind) {
    case ExprKind::Number:
        out += "(num " + expr.text + ")";
        return;
    case ExprKind::String:
        out += "(str ";
        append_quoted(out, expr.text);
        out.push_back(')');
        return;
    case ExprKind::Template:
        out += "(template";
        for (std::size_t i = 0; i < expr.template_strings.size(); ++i) {
            out.push_back(' ');
            append_quoted(out, expr.template_strings[i]);
            if (i < expr.operands.size()) {
                out.push_back(' ');
                dump_expr(out, *expr.operands[i]);
            }
        }
        out.push_back(')');
        return;
    case ExprKind::Boolean:
        out += expr.boolean ? "(true)" : "(false)";
        return;
    case ExprKind::Null:
        out += "(null)";
        return;
    case ExprKind::Identifier:
        out += "(id " + expr.text + ")";
        return;
    case ExprKind::Unary:
        out += "(unary " + expr.text;
        break;
    case ExprKind::Binary:
        out += "(binary " + expr.text;
        break;
    case ExprKind::Conditional:
        out += "(cond";
        break;
    case ExprKind::Member:
        out += expr.text == "[]" ? "(index" : "(member " + expr.text;
        break;
    case ExprKind::Call:
        out += "(call";
        if (!expr.type_args.empty()) {
            out += " ";
            dump_expr(out, *expr.operands[0]);
            out += " <";
            for (std::size_t i = 0; i < expr.type_args.size(); ++i) {
                out += (i == 0 ? "" : ",") + dump(*expr.type_args[i]);
            }
            out += ">";
            dump_operands(out, expr.operands, 1);
            out.push_back(')');
            return;
        }
        break;
    case ExprKind::Return:
        out += "(return";
        break;
    case ExprKind::Array:
        out += "(array";
        break;
    case ExprKind::Object:
        out += "(object";
        for (const auto& property : expr.properties) {
            out += " (";
            if (property.key_is_string) {
                append_quoted(out, property.key);
            } else {
                out += property.key;
            }
            out.push_back(' ');
            dump_expr(out, *property.value);
            out.push_back(')');
        }
        out.push_back(')');
        return;
    case ExprKind::Jsx:
        dump_jsx(out, *expr.jsx);
        return;
    case ExprKind::Arrow: {
        out += "(arrow (";
        for (std::size_t i = 0; i < expr.params.size(); ++i) {
            out += (i == 0 ? "" : " ") + expr.params[i];
            if (i < expr.param_types.size() && expr.param_types[i]) {
                out += ":" + dump(*expr.param_types[i]);
            }
        }
        out.push_back(')');
        if (expr.return_type) {
            out += " -> " + dump(*expr.return_type);
        }
        if (expr.arrow_block_body) {
            out += " (block";
            dump_operands(out, expr.operands);
            out.push_back(')');
        } else {
            dump_operands(out, expr.operands);
        }
        out.push_back(')');
        return;
    }
    }
    dump_operands(out, expr.operands);
    out.push_back(')');
}

} // namespace

std::string dump(const Expr& expr) {
    std::string out;
    dump_expr(out, expr);
    return out;
}

std::string dump(const JsxNode& node) {
    std::string out;
    dump_jsx(out, node);
    return out;
}

} // namespace tessera::compiler::jsx
