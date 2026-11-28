// SPDX-License-Identifier: MIT
//
// Statement lists and the block/object decision of the formatter builder.

#include "builder.hpp"
#include "code_util.hpp"

#include <algorithm>
#include <utility>

namespace tessera::compiler::format {

using namespace code;

bool Builder::starts_statement(std::size_t index) const {
    const FTok& tok = s_.tokens[index];
    if (index == 0 || tok.kind != FKind::Word || !statement_keyword(tok.text)) {
        return false;
    }
    const FTok& prev = s_.tokens[index - 1];
    if (tok.begin <= prev.end ||
        src_.substr(prev.end, tok.begin - prev.end).find('\n') == std::string_view::npos) {
        return false;
    }
    return prev.is(";") || ends_value(prev);
}

bool Builder::is_block(std::size_t opener) const {
    if (opener == 0) {
        return false;
    }
    const FTok& prev = s_.tokens[opener - 1];
    if (prev.is(")") || prev.is("=>") ||
        (prev.kind == FKind::Word && in_set(prev.text, {"else", "try", "finally", "do"}))) {
        return true;
    }
    if (prev.is(":")) {
        // `case x: {` is a block; `a: {` is an object or a type. A statement keyword decides.
        const std::size_t close = static_cast<std::size_t>(s_.tokens[opener].match);
        return close > opener + 1 && s_.tokens[opener + 1].kind == FKind::Word &&
               statement_keyword(s_.tokens[opener + 1].text);
    }
    // A function's return type may sit between `)` and the body: `function F(): Foo {`.
    std::size_t k = opener;
    while (k > 0) {
        --k;
        const FTok& tok = s_.tokens[k];
        if (tok.kind == FKind::Punct && (tok.text == ")" || tok.text == "]" || tok.text == "}") &&
            tok.match >= 0) {
            k = static_cast<std::size_t>(tok.match);
            continue;
        }
        if (tok.is_word("function")) {
            return true;
        }
        if (tok.kind == FKind::Punct && in_set(tok.text, {";", "{", "(", "[", "=", ",", "=>"})) {
            break;
        }
    }
    const std::size_t close = static_cast<std::size_t>(s_.tokens[opener].match);
    return close > opener + 1 && s_.tokens[opener + 1].kind == FKind::Word &&
           s_.tokens[opener + 1].keyword &&
           in_set(s_.tokens[opener + 1].text,
                  {"return", "const", "if", "for", "while", "switch", "throw", "try"});
}

bool Builder::has_top_level_arrow(std::size_t begin, std::size_t end) const {
    for (std::size_t i = begin; i < end; ++i) {
        const FTok& tok = s_.tokens[i];
        if (tok.is("=>")) {
            return true;
        }
        if (tok.kind == FKind::Punct && tok.match > static_cast<std::int32_t>(i) &&
            (tok.text == "(" || tok.text == "[" || tok.text == "{")) {
            i = static_cast<std::size_t>(tok.match);
        }
    }
    return false;
}

std::vector<Builder::Stmt> Builder::split_statements(std::size_t begin, std::size_t end) {
    std::vector<Stmt> out;
    std::size_t s = begin;
    while (s < end) {
        const FTok& first = s_.tokens[s];
        const bool label =
            first.kind == FKind::Word && (first.text == "case" || first.text == "default");
        bool blocky = first.kind == FKind::Word &&
                      in_set(first.text, {"if", "for", "while", "switch", "function", "try", "do"});
        if (first.is_word("export") && s + 1 < end) {
            const FTok& next = s_.tokens[s + 1];
            blocky = next.is_word("function") || (next.is_word("default") && s + 2 < end &&
                                                  s_.tokens[s + 2].is_word("function"));
        }
        const bool conditional_head =
            first.kind == FKind::Word && in_set(first.text, {"if", "for", "while"});
        std::size_t stop = end;
        int ternary = 0;
        for (std::size_t j = s; j < end; ++j) {
            const FTok& tok = s_.tokens[j];
            if (j > s && starts_statement(j) && !(conditional_head && s_.tokens[j - 1].is(")"))) {
                stop = j;
                break;
            }
            if (tok.kind == FKind::Punct && tok.match > static_cast<std::int32_t>(j) &&
                (tok.text == "(" || tok.text == "[" || tok.text == "{")) {
                const bool block = tok.text == "{" && is_block(j);
                j = static_cast<std::size_t>(tok.match);
                // A block ends the statement unless the statement goes on (`else`, `;`, `)`...).
                if (block && j + 1 < end && !continues_statement(s_.tokens[j + 1]) &&
                    (blocky || s_.tokens[j + 1].kind == FKind::Word)) {
                    stop = j + 1;
                    break;
                }
                continue;
            }
            if (tok.is(";")) {
                stop = j + 1;
                break;
            }
            if (label) {
                if (tok.is("?")) {
                    ++ternary;
                } else if (tok.is(":")) {
                    if (ternary == 0) {
                        stop = j + 1;
                        break;
                    }
                    --ternary;
                }
            }
        }
        out.push_back(Stmt{s, stop, label});
        s = stop;
    }
    return out;
}

DocId Builder::statements(std::size_t begin, std::size_t end) {
    const std::vector<Stmt> list = split_statements(begin, end);
    std::vector<DocId> top;
    std::vector<DocId> body; // statements nested under the last case label
    bool in_label = false;
    auto flush = [&]() {
        if (!body.empty()) {
            top.push_back(a_.indent(a_.concat(std::move(body))));
            body.clear();
        }
    };
    for (std::size_t k = 0; k < list.size(); ++k) {
        const Stmt& stmt = list[k];
        std::vector<DocId> unit;
        if (k > 0) {
            unit.push_back(a_.hardline());
            if (first_blank(stmt.begin)) {
                unit.push_back(a_.hardline());
            }
        }
        unit.push_back(lead(stmt.begin));
        unit.push_back(run(stmt.begin, stmt.end));
        const DocId doc = a_.concat(std::move(unit));
        if (stmt.label) {
            flush();
            top.push_back(doc);
            in_label = true;
        } else if (in_label) {
            // Each unit starts with its own newline, so inside the indent a case body sits one
            // level in.
            body.push_back(doc);
        } else {
            top.push_back(doc);
        }
    }
    flush();
    return a_.concat(std::move(top));
}

} // namespace tessera::compiler::format
