// SPDX-License-Identifier: MIT

// `// Note: bind::<id>` annotations (docs/specs/jsx-subset-v1.md sections 7 and 9.3).
//
// The lexer keeps every comment. After each top-level item is parsed, the comments inside it are
// paired with the statement on the next line. Items that already have errors are not paired, so
// a broken component does not add bind diagnostics of its own.

#include "parser_internal.hpp"

#include <optional>

namespace tessera::compiler::jsx {
namespace {

// The text after `bind::` when the comment is a `// Note: bind::...` line comment.
std::optional<std::string> bind_annotation(const Comment& comment) {
    if (comment.block) {
        return std::nullopt;
    }
    std::string_view text = comment.text;
    const auto trim = [](std::string_view value) {
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            value.remove_prefix(1);
        }
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
            value.remove_suffix(1);
        }
        return value;
    };
    text = trim(text);
    if (!text.starts_with("Note:")) {
        return std::nullopt;
    }
    text = trim(text.substr(5));
    if (!text.starts_with("bind::")) {
        return std::nullopt;
    }
    return std::string(text.substr(6));
}

bool valid_bind_id(const std::string& id) {
    if (id.empty() || id[0] < 'a' || id[0] > 'z') {
        return false;
    }
    for (const char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool is_use_effect_call(const Expr& expr) {
    return expr.kind == ExprKind::Call && !expr.operands.empty() &&
           expr.operands[0]->kind == ExprKind::Identifier && expr.operands[0]->text == "useEffect";
}

// A handler declaration (`const f = <arrow>;`) or a `useEffect(...)` statement.
bool accepts_bind(const Statement& statement) {
    if (statement.kind == StatementKind::Const) {
        return !statement.array_pattern && statement.names.size() == 1 && statement.init &&
               statement.init->kind == ExprKind::Arrow;
    }
    return statement.init && is_use_effect_call(*statement.init);
}

// `useEffect(() => { ...; return () => {...}; }, deps)` has a cleanup handler (spec section 7).
bool effect_has_cleanup(const Expr& call) {
    if (call.operands.size() < 2 || call.operands[1]->kind != ExprKind::Arrow) {
        return false;
    }
    const Expr& body = *call.operands[1];
    if (!body.arrow_block_body || body.operands.empty()) {
        return false;
    }
    const Expr& last = *body.operands.back();
    return last.kind == ExprKind::Return && !last.operands.empty() &&
           last.operands[0]->kind == ExprKind::Arrow;
}

} // namespace

void Parser::check_item_comments(ModuleItem& item, bool pair_inside) {
    const auto& comments = lexer_.comments();
    const std::uint32_t item_end = item.span.end();
    std::size_t statement_index = 0;
    for (; comment_cursor_ < comments.size() && comments[comment_cursor_].span.offset < item_end;
         ++comment_cursor_) {
        const Comment& comment = comments[comment_cursor_];
        const auto id = bind_annotation(comment);
        if (!id) {
            continue;
        }
        const bool inside = comment.span.offset >= item.span.offset;
        if (inside && !pair_inside) {
            continue; // the item is already invalid
        }
        const bool in_component = inside && item.kind == ItemKind::Component;
        if (!in_component) {
            report("TESSERA_REACT_BIND_POSITION",
                   "`bind::` annotations go before a handler declaration or a `useEffect` "
                   "statement inside a component",
                   comment.span, "move the comment (spec section 9.3)");
            continue;
        }
        if (!valid_bind_id(*id)) {
            report("TESSERA_REACT_BIND_ID", "malformed bind id `" + *id + "`", comment.span,
                   "use `[a-z][a-z0-9_]*`");
            continue;
        }
        // Statements and comments are both in source order, so one forward scan finds the
        // statement that follows this comment.
        while (statement_index < item.body.size() &&
               item.body[statement_index].span.offset < comment.span.end()) {
            ++statement_index;
        }
        Statement* target = nullptr;
        if (statement_index < item.body.size()) {
            Statement& next = item.body[statement_index];
            const bool previous_line =
                map_.locate(comment.span.end()).line + 1 == map_.locate(next.span.offset).line;
            if (previous_line && accepts_bind(next)) {
                target = &next;
            }
        }
        if (target == nullptr) {
            report("TESSERA_REACT_BIND_POSITION",
                   "`bind::" + *id +
                       "` must be on the line before a handler declaration or a "
                       "`useEffect` statement",
                   comment.span, "move the comment (spec section 9.3)");
            continue;
        }
        if (!bind_ids_.insert(*id).second) {
            report("TESSERA_REACT_BIND_ID", "duplicate bind id `" + *id + "`", comment.span,
                   "bind ids are unique in the project");
            continue;
        }
        if (target->kind == StatementKind::Expression && effect_has_cleanup(*target->init) &&
            !bind_ids_.insert(*id + "_cleanup").second) {
            report("TESSERA_REACT_BIND_ID",
                   "`bind::" + *id + "` reserves `" + *id + "_cleanup`, which is already used",
                   comment.span, "rename one of the ids");
            continue;
        }
        target->bind_id = *id;
        target->bind_span = comment.span;
    }
}

// Comments after the last item, and comments the recovery skipped, are not inside any item.
void Parser::check_remaining_comments() {
    const auto& comments = lexer_.comments();
    for (; comment_cursor_ < comments.size(); ++comment_cursor_) {
        if (bind_annotation(comments[comment_cursor_])) {
            report("TESSERA_REACT_BIND_POSITION",
                   "`bind::` annotations go before a handler declaration or a `useEffect` "
                   "statement inside a component",
                   comments[comment_cursor_].span, "move the comment (spec section 9.3)");
        }
    }
}

// After recovery skipped tokens, the comments among them belong to the invalid item.
void Parser::skip_comments_before_current() {
    const auto& comments = lexer_.comments();
    while (comment_cursor_ < comments.size() &&
           comments[comment_cursor_].span.offset < cur_.span.offset) {
        ++comment_cursor_;
    }
}

} // namespace tessera::compiler::jsx
