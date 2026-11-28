// SPDX-License-Identifier: MIT

#include "file_indexer.hpp"

#include <algorithm>
#include <cctype>

namespace tessera::compiler::analysis {

namespace {

bool is_upper(std::string_view text) {
    return !text.empty() && std::isupper(static_cast<unsigned char>(text.front())) != 0;
}

} // namespace

void FileIndexer::walk(const jsx::Expr* expr) {
    if (expr == nullptr) {
        return;
    }
    using jsx::ExprKind;
    switch (expr->kind) {
    case ExprKind::Identifier:
        reference_value(expr->text, span_of(expr->span));
        return;
    case ExprKind::Member:
    case ExprKind::OptionalMember:
        walk_member(*expr);
        return;
    case ExprKind::Call:
        for (const auto& operand : expr->operands) {
            walk(operand.get());
        }
        for (const auto& type : expr->type_args) {
            walk_type(type.get());
        }
        return;
    case ExprKind::Arrow:
        walk_arrow(*expr);
        return;
    case ExprKind::Block: {
        push_scope(anonymous_label());
        for (const auto& operand : expr->operands) {
            walk(operand.get());
        }
        pop_scope();
        return;
    }
    case ExprKind::Const: {
        // The initializer is read before the name exists: `const x = x` names an outer x.
        for (const auto& operand : expr->operands) {
            walk(operand.get());
        }
        // The node's own span can end at `const`; the name sits before the initializer.
        jsx::Span header = expr->span;
        if (!expr->operands.empty() && expr->operands.front() != nullptr) {
            header.length = expr->operands.front()->span.offset - header.offset;
        }
        std::uint32_t cursor = header.offset;
        if (const auto keyword = find_ident("const", cursor, header.end())) {
            cursor = *keyword + 5;
        }
        const bool handler = !expr->operands.empty() && expr->operands.front() != nullptr &&
                             expr->operands.front()->kind == ExprKind::Arrow;
        declare(expr->text, handler ? SymbolKind::Handler : SymbolKind::Local,
                next_name(expr->text, header, &cursor));
        return;
    }
    case ExprKind::Object:
        walk(expr->spread_base.get());
        for (const auto& property : expr->properties) {
            walk(property.value.get());
        }
        return;
    case ExprKind::Jsx:
        walk_jsx(expr->jsx.get());
        return;
    default:
        for (const auto& operand : expr->operands) {
            walk(operand.get());
        }
        return;
    }
}

void FileIndexer::walk_arrow(const jsx::Expr& arrow) {
    push_scope(anonymous_label());
    std::uint32_t cursor = arrow.span.offset;
    for (std::size_t i = 0; i < arrow.params.size(); ++i) {
        declare(arrow.params[i], SymbolKind::Param, next_name(arrow.params[i], arrow.span, &cursor));
        if (i < arrow.param_types.size()) {
            walk_type(arrow.param_types[i].get());
        }
    }
    walk_type(arrow.return_type.get());
    for (const auto& operand : arrow.operands) {
        walk(operand.get());
    }
    pop_scope();
}

void FileIndexer::walk_member(const jsx::Expr& member) {
    if (member.operands.empty()) {
        return;
    }
    const jsx::Expr* object = member.operands.front().get();
    walk(object);
    if (member.text == "[]") {
        if (member.operands.size() > 1) {
            walk(member.operands[1].get());
        }
        return;
    }
    // The property is the last token of the expression; a parenthesized or otherwise unusual
    // shape that does not end in the name is skipped rather than guessed.
    if (member.span.length < member.text.size()) {
        return;
    }
    const std::uint32_t start = member.span.end() - static_cast<std::uint32_t>(member.text.size());
    if (source_.substr(start, member.text.size()) != member.text) {
        return;
    }
    const Span span = span_at(start, member.text.size());
    if (object != nullptr && object->kind == jsx::ExprKind::Identifier) {
        if (const auto binding = lookup(object->text);
            binding && out_.symbols[*binding].kind == SymbolKind::CssModule) {
            add_reference(span, ReferenceKind::CssClass, member.text, Resolution{{}, Unresolved::External},
                          out_.symbols[*binding].id);
            return;
        }
    }
    add_reference(span, ReferenceKind::Member, member.text, Resolution{{}, Unresolved::NeedsType});
}

void FileIndexer::walk_jsx(const jsx::JsxNode* node) {
    if (node == nullptr) {
        return;
    }
    using jsx::JsxKind;
    if (node->kind == JsxKind::ExprContainer) {
        walk(node->expr.get());
        return;
    }
    if (node->kind == JsxKind::Element) {
        const bool qualified = node->tag.find_first_of(".:") != std::string::npos;
        std::optional<std::size_t> tag_symbol;
        const bool component = !qualified && is_upper(node->tag);
        if (component) {
            tag_symbol = lookup(node->tag);
        }
        auto tag_resolution = [&]() {
            if (!qualified && !component) {
                return Resolution{{}, Unresolved::Intrinsic};
            }
            return tag_symbol ? Resolution{out_.symbols[*tag_symbol].id, {}}
                              : Resolution{{}, Unresolved::Unbound};
        };
        if (!qualified) {
            add_reference(span_of(node->tag_span), ReferenceKind::Tag, node->tag, tag_resolution());
            // The closing tag has no span in the AST; it ends the element, so find it from there.
            if (!node->self_closing && node->span.length > node->tag.size() + 3) {
                const std::size_t close = source_.rfind("</", node->span.end() - 1);
                if (close != std::string_view::npos && close >= node->span.offset &&
                    source_.substr(close + 2, node->tag.size()) == node->tag) {
                    add_reference(span_at(static_cast<std::uint32_t>(close) + 2, node->tag.size()),
                                  ReferenceKind::Tag, node->tag, tag_resolution());
                }
            }
        }
        for (const jsx::JsxAttribute& attribute : node->attributes) {
            if (attribute.kind != jsx::JsxAttrKind::Spread && !attribute.name.empty()) {
                Resolution resolution{{}, Unresolved::Unbound};
                std::string via;
                if (!qualified && !component) {
                    resolution.reason = Unresolved::Intrinsic;
                } else if (tag_symbol) {
                    // The project pass looks the prop up on the component the tag stands for.
                    resolution.reason = Unresolved::NeedsType;
                    via = out_.symbols[*tag_symbol].id;
                }
                add_reference(span_of(attribute.name_span), ReferenceKind::Attribute, attribute.name,
                              resolution, via);
            }
            walk(attribute.expr.get());
        }
    }
    for (const auto& child : node->children) {
        walk_jsx(child.get());
    }
}

void FileIndexer::walk_type(const jsx::TypeNode* type) {
    if (type == nullptr) {
        return;
    }
    if (type->kind == jsx::TypeKind::Name && !type->text.empty()) {
        static constexpr std::string_view kBuiltins[] = {"int",  "float", "string", "bool", "color",
                                                         "null", "void",  "Array",  "optional"};
        Resolution resolution{{}, Unresolved::Unbound};
        if (const auto found = lookup_type(type->text)) {
            resolution = Resolution{out_.symbols[*found].id, {}};
        } else if (std::ranges::find(kBuiltins, type->text) != std::end(kBuiltins)) {
            resolution.reason = Unresolved::Builtin;
        }
        add_reference(span_at(type->span.offset, type->text.size()), ReferenceKind::Type, type->text,
                      std::move(resolution));
    }
    for (const auto& child : type->children) {
        walk_type(child.get());
    }
    for (const jsx::TypeMember& member : type->members) {
        walk_type(member.type.get());
    }
}

} // namespace tessera::compiler::analysis
