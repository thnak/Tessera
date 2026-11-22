// SPDX-License-Identifier: MIT

// The reducer form of ADR-066 item 4, as the parser sees it: which module-scope helper body
// declares a switch, and how that body is parsed. Split out of parser_component.cpp when that
// file reached the 500-line hard limit; the subject is the reducer, not the component around it.
//
// A helper whose body has no switch is skipped wholesale (issue #660's fix for unrelated
// helpers), because a helper is not lowered and may use forms the subset does not have. A helper
// whose body declares a switch is the reducer the lowering reads, so its body is parsed as a
// statement list and recorded on the item.

#include "parser_internal.hpp"

namespace tessera::compiler::jsx {

// ADR-066 item 4: whether the helper body about to be parsed declares a `switch`. A reducer is the
// only helper whose body the lowering reads, and it is recognised by that form. The scan is a
// lookahead: it saves the lexer position, walks the body's braces, and restores, so the body is
// parsed once either way. The caller has already consumed the body's opening `{`, so the scan
// starts one level in.
bool Parser::helper_has_switch() {
    const Checkpoint saved = checkpoint();
    bool found = false;
    int depth = 1;
    while (!fatal_ && cur_.kind != TokenKind::Eof && depth > 0) {
        if (at_punct("{")) {
            ++depth;
        } else if (at_punct("}")) {
            --depth;
            if (depth == 0) {
                break;
            }
        } else if (cur_.text == "switch") {
            found = true;
            break;
        }
        advance(LexMode::Code);
    }
    restore(saved);
    return found;
}

// ADR-066 item 4: the body of a reducer helper. It is a statement list whose one statement is the
// `switch`; the statements are recorded on the item so the lowering can read them.
bool Parser::parse_reducer_body(ModuleItem& item) {
    while (!fatal_ && !at_punct("}")) {
        ExprPtr statement = parse_block_statement();
        if (!statement) {
            return false;
        }
        Statement recorded;
        recorded.kind = StatementKind::Expression;
        recorded.span = statement->span;
        recorded.init = std::move(statement);
        item.body.push_back(std::move(recorded));
        if (at_punct(";")) {
            advance(LexMode::Code);
        }
    }
    if (fatal_) {
        return false;
    }
    if (!at_punct("}")) {
        fail_unexpected("`}`");
        return false;
    }
    return true;
}

} // namespace tessera::compiler::jsx
