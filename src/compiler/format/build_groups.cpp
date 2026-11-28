// SPDX-License-Identifier: MIT
//
// Bracket groups of the formatter builder: lists, objects, blocks and parentheses.

#include "builder.hpp"
#include "code_util.hpp"

#include <algorithm>
#include <utility>

namespace tessera::compiler::format {

using namespace code;

DocId Builder::group_doc(std::size_t opener) {
    const FTok& t = s_.tokens[opener];
    if (t.text == "(") {
        return paren_doc(opener);
    }
    if (t.text == "[") {
        return list_doc(opener, true);
    }
    return is_block(opener) ? block_doc(opener) : curly_doc(opener);
}

std::vector<std::pair<std::size_t, std::size_t>>
Builder::split_items(std::size_t begin, std::size_t end, bool semicolons) {
    // The pair is (item begin, separator index or kNone); the item ends at the separator.
    std::vector<std::pair<std::size_t, std::size_t>> items;
    std::size_t start = begin;
    for (std::size_t i = begin; i < end; ++i) {
        const FTok& tok = s_.tokens[i];
        if (tok.kind == FKind::Punct && tok.match > static_cast<std::int32_t>(i) &&
            (tok.text == "(" || tok.text == "[" || tok.text == "{")) {
            i = static_cast<std::size_t>(tok.match);
        } else if (tok.is(",") || (semicolons && tok.is(";"))) {
            items.emplace_back(start, i);
            start = i + 1;
        }
    }
    if (start < end) {
        items.emplace_back(start, kNone);
    }
    return items;
}

DocId Builder::join_items(const std::vector<DocId>& items, DocId separator) {
    std::vector<DocId> parts;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) {
            parts.push_back(separator);
        }
        parts.push_back(items[i]);
    }
    return a_.concat(std::move(parts));
}

DocId Builder::dangling(std::size_t opener, std::size_t closer, std::string_view open_text) {
    const FTok& close = s_.tokens[closer];
    const bool own_line = !close.leading.empty();
    bool all_block = true;
    for (const std::uint32_t id : s_.tokens[opener].trailing) {
        all_block = all_block && s_.comments[id].block;
    }
    std::string close_text(close.text);
    if (s_.tokens[opener].trailing.empty() && !own_line) {
        return a_.concat(
            {a_.text(std::string(open_text) + close_text), trailing_docs(closer, true)});
    }
    if (all_block && !own_line) {
        std::vector<DocId> parts{a_.text(std::string(open_text))};
        const auto& trailing = s_.tokens[opener].trailing;
        for (std::size_t i = 0; i < trailing.size(); ++i) {
            if (i > 0) {
                parts.push_back(a_.text(" "));
            }
            parts.push_back(comment_doc(trailing[i]));
        }
        parts.push_back(a_.text(close_text));
        parts.push_back(trailing_docs(closer, true));
        return a_.concat(std::move(parts));
    }
    std::vector<DocId> inner;
    for (const std::uint32_t id : s_.tokens[opener].trailing) {
        inner.push_back(a_.hardline());
        inner.push_back(comment_doc(id));
    }
    inner.push_back(lead_close(closer));
    return a_.concat({a_.text(std::string(open_text)), a_.indent(a_.concat(std::move(inner))),
                      a_.hardline(), a_.text(close_text), trailing_docs(closer, true)});
}

// Items of a bracket list: each on its own line when the group breaks, a trailing comma then
// (arrays and objects only; call arguments and parameters keep none, the subset has no such form).
DocId Builder::list_doc(std::size_t opener, bool trailing_comma) {
    const std::size_t closer = static_cast<std::size_t>(s_.tokens[opener].match);
    const bool paren = s_.tokens[opener].text == "(";
    const auto items = split_items(opener + 1, closer, false);
    const std::string open_text(s_.tokens[opener].text);
    if (items.empty()) {
        return dangling(opener, closer, open_text);
    }
    std::vector<DocId> docs;
    std::vector<DocId> seps;
    for (std::size_t k = 0; k < items.size(); ++k) {
        const std::size_t item_end = items[k].second == kNone ? closer : items[k].second;
        if (items[k].first >= item_end) {
            fail("empty list element", s_.tokens[items[k].first].begin);
            return a_.empty();
        }
        std::vector<DocId> pieces;
        if (k == 0) {
            for (const DocId d : opener_comments(opener, false)) {
                pieces.push_back(d);
            }
        }
        pieces.push_back(lead(items[k].first));
        pieces.push_back(run(items[k].first, item_end));
        docs.push_back(a_.concat(std::move(pieces)));
        if (items[k].second != kNone) {
            seps.push_back(a_.concat({a_.text(","), trailing_docs(items[k].second, true)}));
        } else {
            seps.push_back(a_.empty());
        }
    }
    const DocId closer_comments = lead_close(closer);
    const bool plain = !a_.forced(closer_comments);

    if (paren && plain && !has_opener_comments(opener)) {
        const std::size_t n = items.size();
        const std::size_t last_begin = items[n - 1].first;
        const FTok& first_tok = s_.tokens[last_begin];
        const bool hug_last =
            (first_tok.is("{") || first_tok.is("[") ||
             has_top_level_arrow(last_begin,
                                 items[n - 1].second == kNone ? closer : items[n - 1].second)) &&
            first_tok.leading.empty();
        bool others_forced = false;
        for (std::size_t k = 0; k + 1 < n; ++k) {
            others_forced = others_forced || a_.forced(docs[k]);
        }
        const bool hug_first =
            n >= 2 && a_.forced(docs[0]) && has_top_level_arrow(items[0].first, items[0].second);
        bool rest_forced = false;
        for (std::size_t k = 1; k < n; ++k) {
            rest_forced = rest_forced || a_.forced(docs[k]);
        }
        if ((hug_last && !others_forced) || (hug_first && !rest_forced)) {
            std::vector<DocId> parts{a_.text("(")};
            for (std::size_t k = 0; k < n; ++k) {
                parts.push_back(docs[k]);
                if (k + 1 < n) {
                    parts.push_back(seps[k]);
                    parts.push_back(a_.text(" "));
                }
            }
            parts.push_back(tok(closer));
            return a_.concat(std::move(parts));
        }
    }

    std::vector<DocId> body;
    for (std::size_t k = 0; k < docs.size(); ++k) {
        body.push_back(docs[k]);
        if (k + 1 < docs.size()) {
            body.push_back(seps[k]);
            body.push_back(a_.line());
            if (first_blank(items[k + 1].first)) {
                body.push_back(a_.if_break(a_.hardline(), a_.empty()));
            }
        }
    }
    std::vector<DocId> tail;
    if (trailing_comma) {
        tail.push_back(a_.if_break(a_.text(","), a_.empty()));
    }
    if (items.back().second != kNone) {
        tail.push_back(trailing_docs(items.back().second, true));
    }
    const DocId inner = a_.concat(
        {a_.softline(), a_.concat(std::move(body)), a_.concat(std::move(tail)), closer_comments});
    return a_.group(a_.concat({a_.text(open_text), a_.concat(opener_comments(opener, true)),
                               a_.indent(inner), a_.softline(), tok(closer)}));
}

DocId Builder::paren_doc(std::size_t opener) {
    const std::size_t closer = static_cast<std::size_t>(s_.tokens[opener].match);
    const FTok& prev = opener > 0 ? s_.tokens[opener - 1] : s_.tokens[opener];
    const bool call =
        opener > 0 && (word_operand(prev) || prev.is(")") || prev.is("]") || prev.is(">"));
    DocId element = 0;
    if (!call && s_.tokens[opener].trailing.empty() && s_.tokens[closer].leading.empty() &&
        single_jsx(opener + 1, closer, element)) {
        return a_.concat({element, trailing_docs(closer, true)});
    }
    return list_doc(opener, false);
}

DocId Builder::curly_doc(std::size_t opener) {
    const std::size_t closer = static_cast<std::size_t>(s_.tokens[opener].match);
    const auto items = split_items(opener + 1, closer, true);
    if (items.empty()) {
        return dangling(opener, closer, "{");
    }
    bool semicolons = false;
    for (const auto& item : items) {
        semicolons = semicolons || (item.second != kNone && s_.tokens[item.second].is(";"));
    }
    const std::string sep_text = semicolons ? ";" : ",";
    std::vector<DocId> body;
    for (std::size_t k = 0; k < items.size(); ++k) {
        const std::size_t item_end = items[k].second == kNone ? closer : items[k].second;
        if (items[k].first >= item_end) {
            fail("empty member", s_.tokens[items[k].first].begin);
            return a_.empty();
        }
        std::vector<DocId> pieces;
        if (k == 0) {
            for (const DocId d : opener_comments(opener, false)) {
                pieces.push_back(d);
            }
        }
        pieces.push_back(lead(items[k].first));
        pieces.push_back(run(items[k].first, item_end));
        if (k + 1 < items.size()) {
            pieces.push_back(a_.text(sep_text));
            pieces.push_back(trailing_docs(items[k].second, true));
        }
        body.push_back(a_.concat(std::move(pieces)));
        if (k + 1 < items.size()) {
            body.push_back(a_.line());
            if (first_blank(items[k + 1].first)) {
                body.push_back(a_.if_break(a_.hardline(), a_.empty()));
            }
        }
    }
    std::vector<DocId> tail{a_.if_break(a_.text(sep_text), a_.empty())};
    if (items.back().second != kNone) {
        tail.push_back(trailing_docs(items.back().second, true));
    }
    const DocId inner = a_.concat(
        {a_.line(), a_.concat(std::move(body)), a_.concat(std::move(tail)), lead_close(closer)});
    return a_.group(a_.concat({a_.text("{"), a_.concat(opener_comments(opener, true)),
                               a_.indent(inner), a_.line(), tok(closer)}));
}

DocId Builder::block_doc(std::size_t opener) {
    const std::size_t closer = static_cast<std::size_t>(s_.tokens[opener].match);
    if (opener + 1 == closer) {
        return dangling(opener, closer, "{");
    }
    std::vector<DocId> inner;
    for (const DocId d : opener_comments(opener, true)) {
        inner.push_back(d);
    }
    for (const DocId d : opener_comments(opener, false)) {
        inner.push_back(d);
    }
    inner.push_back(a_.hardline());
    inner.push_back(statements(opener + 1, closer));
    inner.push_back(lead_close(closer));
    return a_.concat(
        {a_.text("{"), a_.indent(a_.concat(std::move(inner))), a_.hardline(), tok(closer)});
}

} // namespace tessera::compiler::format
