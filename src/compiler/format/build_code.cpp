// SPDX-License-Identifier: MIT
//
// Code-mode runs of the formatter builder: tokens with canonical spacing.

#include "builder.hpp"
#include "code_util.hpp"

#include <algorithm>
#include <utility>

namespace tessera::compiler::format {

using namespace code;

// Spacing between two neighbouring elements of a run. `<`/`>` keep the author's spelling because
// the same characters are generic brackets and comparisons.
namespace {

struct SpaceState {
    bool prev_operand = false;
    bool prev_unary = false;
    int ternary = 0;
};

bool needs_space(const std::vector<FTok>& toks, std::size_t prev_last, bool prev_is_token,
                 bool prev_closer_call, std::size_t cur_first, bool cur_is_group, bool cur_is_jsx,
                 SpaceState& st) {
    const FTok& p = toks[prev_last];
    const FTok& c = toks[cur_first];
    if (!cur_is_group && !cur_is_jsx && c.kind == FKind::Punct) {
        const std::string_view t = c.text;
        if (t == "," || t == ";" || t == "." || t == "?.") {
            return false;
        }
        if (t == ":") {
            if (st.ternary > 0) {
                --st.ternary;
                return true;
            }
            return false;
        }
        if (t == "?") {
            const FTok& next = toks[cur_first + 1];
            if (next.is(":") && next.begin == c.end) {
                return false;
            }
            ++st.ternary;
            return true;
        }
        if (c.generic) {
            return false;
        }
        if (is_binary(t) || t == "<" || t == ">" || t == ">>" || t == ">>>") {
            return true;
        }
    }
    if (prev_is_token && p.kind == FKind::Punct) {
        const std::string_view t = p.text;
        if (t == "." || t == "?." || t == "...") {
            return false;
        }
        if (st.prev_unary || (p.generic && t == "<")) {
            return false;
        }
    }
    if (cur_is_group) {
        const std::string_view t = c.text;
        const bool call_prev = prev_closer_call || (prev_is_token && word_operand(p));
        if (t == "(" || t == "[") {
            if (call_prev) {
                return false;
            }
            return !(prev_is_token && p.generic && p.text != "<");
        }
    }
    return true;
}

} // namespace

DocId Builder::run(std::size_t begin, std::size_t end) {
    std::vector<DocId> parts;
    SpaceState st;
    // A logical chain may break after its operators, except around JSX, where the element's own
    // parentheses decide the layout.
    bool has_jsx = false;
    for (std::size_t k = begin; k < end; ++k) {
        has_jsx = has_jsx || s_.tokens[k].jsx_lt;
    }
    std::size_t tail_begin = kNone;
    bool prev_logical = false;
    bool have_prev = false;
    std::size_t prev_last = 0;
    bool prev_token = false;
    bool prev_call_closer = false;
    for (std::size_t i = begin; i < end && error_.empty();) {
        const FTok& t = s_.tokens[i];
        std::size_t last = i;
        DocId doc = 0;
        bool is_group = false;
        bool is_jsx = false;
        if (t.kind == FKind::Punct && t.match > static_cast<std::int32_t>(i) &&
            (t.text == "(" || t.text == "[" || t.text == "{")) {
            last = static_cast<std::size_t>(t.match);
            is_group = true;
        } else if (t.jsx_lt && t.mode == jsx::LexMode::Code) {
            is_jsx = true;
        }
        if (is_jsx) {
            bool forced = false;
            last = jsx_element(i, doc, forced) - 1;
        } else if (is_group) {
            doc = group_doc(i);
        } else {
            doc = tok(i);
        }
        const bool group_like = is_group;
        if (have_prev) {
            if (needs_space(s_.tokens, prev_last, prev_token, prev_call_closer, i, group_like,
                            is_jsx, st)) {
                if (prev_logical && !has_jsx) {
                    if (tail_begin == kNone) {
                        tail_begin = parts.size();
                    }
                    parts.push_back(a_.line());
                } else {
                    parts.push_back(a_.text(" "));
                }
            }
        }
        if (is_jsx && have_prev && !st.prev_operand) {
            const FTok& p = s_.tokens[prev_last];
            if (p.kind == FKind::Punct || (p.kind == FKind::Word && p.keyword)) {
                doc = jsx_wrapped(doc);
            }
        }
        parts.push_back(doc);
        prev_logical =
            !is_group && !is_jsx && t.kind == FKind::Punct && in_set(t.text, {"&&", "||", "??"});
        // An arrow's expression body moves to its own line when the arrow does not fit; a block,
        // a parenthesised body and a JSX body keep the `=>` line.
        if (!is_group && !is_jsx && t.is("=>") && i + 1 < end) {
            const FTok& body = s_.tokens[i + 1];
            const bool keep_inline =
                body.jsx_lt || (body.is("{") && is_block(i + 1)) ||
                (body.is("(") && static_cast<std::size_t>(body.match) + 1 == end);
            if (!keep_inline) {
                parts.push_back(a_.group(a_.indent(a_.concat({a_.line(), run(i + 1, end)}))));
                break;
            }
        }
        // state for the next element
        st.prev_unary = !is_group && !is_jsx && t.kind == FKind::Punct &&
                        in_set(t.text, {"!", "~", "-", "+"}) && !st.prev_operand;
        if (is_group) {
            st.prev_operand = true;
            prev_call_closer = t.text == "(" || t.text == "[";
            prev_token = false;
        } else if (is_jsx) {
            st.prev_operand = true;
            prev_call_closer = false;
            prev_token = false;
        } else {
            st.prev_operand = word_operand(t) || t.kind == FKind::Number ||
                              t.kind == FKind::String || t.kind == FKind::Atom;
            prev_call_closer = false;
            prev_token = true;
        }
        have_prev = true;
        prev_last = last;
        i = last + 1;
    }
    if (tail_begin != kNone) {
        std::vector<DocId> head(parts.begin(),
                                parts.begin() + static_cast<std::ptrdiff_t>(tail_begin));
        std::vector<DocId> tail(parts.begin() + static_cast<std::ptrdiff_t>(tail_begin),
                                parts.end());
        head.push_back(a_.indent(a_.concat(std::move(tail))));
        return a_.group(a_.concat(std::move(head)));
    }
    return a_.concat(std::move(parts));
}

} // namespace tessera::compiler::format
