// SPDX-License-Identifier: MIT
//
// JSX layout for the formatter. Whitespace inside JSX text is meaningful, so the printer only
// ever breaks where JSX itself drops the whitespace: between elements and expression containers,
// and between the words of a text run (a newline there collapses to the single space it replaces).

#include "builder.hpp"

#include <algorithm>
#include <utility>

namespace tessera::compiler::format {
namespace {

constexpr std::size_t kNone = static_cast<std::size_t>(-1);

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

struct Part {
    DocId doc = 0;
    std::string lead_glue;  // significant whitespace at the start of a text run
    std::string trail_glue; // and at its end
    bool glue_only = false; // whitespace between two nodes, with no newline in it
    bool text = false;
    bool node = false;         // an element or an expression container
    bool blank_before = false; // the source had a blank line before this part
};

} // namespace

DocId Builder::jsx_wrapped(DocId element) {
    return a_.group(a_.concat({a_.if_break(a_.text("("), a_.empty()),
                               a_.indent(a_.concat({a_.softline(), element})), a_.softline(),
                               a_.if_break(a_.text(")"), a_.empty())}));
}

std::size_t Builder::jsx_extent(std::size_t index) const {
    const auto& toks = s_.tokens;
    int depth = 0;
    std::size_t j = index;
    while (j < toks.size()) {
        const FTok& t = toks[j];
        if (t.jsx_lt) {
            const bool closing =
                j + 1 < toks.size() && (toks[j + 1].is("/") || toks[j + 1].is("/>"));
            std::size_t k = j + 1;
            while (k < toks.size() && !(toks[k].is(">") || toks[k].is("/>"))) {
                if (toks[k].is("{") && toks[k].match > 0) {
                    k = static_cast<std::size_t>(toks[k].match);
                }
                ++k;
            }
            if (k >= toks.size()) {
                return kNone;
            }
            if (closing) {
                --depth;
            } else if (toks[k].is(">")) {
                ++depth;
            }
            j = k + 1;
            if (depth <= 0) {
                return j;
            }
            continue;
        }
        if (t.is("{") && t.match > 0) {
            j = static_cast<std::size_t>(t.match) + 1;
        } else {
            ++j;
        }
    }
    return kNone;
}

bool Builder::single_jsx(std::size_t begin, std::size_t end, DocId& out) {
    if (begin >= end || !s_.tokens[begin].jsx_lt || s_.tokens[begin].mode != jsx::LexMode::Code ||
        jsx_extent(begin) != end) {
        return false;
    }
    bool forced = false;
    jsx_element(begin, out, forced);
    out = jsx_wrapped(out);
    return true;
}

std::string Builder::collect_name(std::size_t& cursor, std::size_t& last) {
    const std::uint32_t begin = s_.tokens[cursor].begin;
    last = cursor;
    ++cursor;
    while ((s_.tokens[cursor].is(".") || s_.tokens[cursor].is(":")) &&
           s_.tokens[cursor + 1].kind == FKind::Word) {
        last = cursor + 1;
        cursor += 2;
    }
    return std::string(src_.substr(begin, s_.tokens[last].end - begin));
}

DocId Builder::jsx_container(std::size_t opener) {
    const std::size_t closer = static_cast<std::size_t>(s_.tokens[opener].match);
    if (opener + 1 == closer) {
        return dangling(opener, closer, "{");
    }
    std::vector<DocId> inner = opener_comments(opener, true);
    for (const DocId d : opener_comments(opener, false)) {
        inner.push_back(d);
    }
    const DocId first = lead(opener + 1);
    inner.push_back(first);
    inner.push_back(run(opener + 1, closer));
    const DocId close_comments = lead_close(closer);
    if (!a_.forced(first) && !a_.forced(close_comments)) {
        return a_.concat({a_.text("{"), a_.concat(std::move(inner)), tok(closer)});
    }
    inner.push_back(close_comments);
    return a_.concat({a_.text("{"),
                      a_.indent(a_.concat({a_.hardline(), a_.concat(std::move(inner))})),
                      a_.hardline(), tok(closer)});
}

DocId Builder::jsx_attribute(std::size_t& cursor) {
    std::vector<DocId> parts{lead(cursor)};
    if (s_.tokens[cursor].is("{")) {
        const std::size_t opener = cursor;
        cursor = static_cast<std::size_t>(s_.tokens[opener].match) + 1;
        parts.push_back(jsx_container(opener));
        return a_.concat(std::move(parts));
    }
    std::size_t last = cursor;
    parts.push_back(a_.text(collect_name(cursor, last)));
    parts.push_back(trailing_docs(last, true));
    if (s_.tokens[cursor].is("=")) {
        parts.push_back(a_.text("="));
        parts.push_back(trailing_docs(cursor, true));
        ++cursor;
        if (s_.tokens[cursor].is("{")) {
            const std::size_t opener = cursor;
            cursor = static_cast<std::size_t>(s_.tokens[opener].match) + 1;
            parts.push_back(jsx_container(opener));
        } else {
            parts.push_back(tok(cursor));
            ++cursor;
        }
    }
    return a_.concat(std::move(parts));
}

// The opening tag, `<name attrs>` or `<name attrs />`, with `cursor` moved past it.
DocId Builder::jsx_open_tag(std::size_t lt, std::size_t& cursor, std::string& name,
                            bool& self_closing) {
    cursor = lt + 1;
    name.clear();
    DocId name_doc = a_.empty();
    if (!s_.tokens[cursor].is(">")) {
        std::size_t last = cursor;
        name = collect_name(cursor, last);
        name_doc = a_.concat({a_.text(name), trailing_docs(last, true)});
    }
    std::vector<DocId> attrs;
    while (error_.empty() && !(s_.tokens[cursor].is(">") || s_.tokens[cursor].is("/>"))) {
        if (s_.tokens[cursor].kind == FKind::Eof) {
            fail("unterminated JSX tag", s_.tokens[lt].begin);
            return a_.empty();
        }
        attrs.push_back(jsx_attribute(cursor));
    }
    self_closing = s_.tokens[cursor].is("/>");
    const DocId close_tail = trailing_docs(cursor, true);
    const DocId lt_doc = a_.concat({a_.text("<"), trailing_docs(lt, true)});
    ++cursor;
    if (attrs.empty()) {
        return a_.concat({lt_doc, name_doc, a_.text(self_closing ? " />" : ">"), close_tail});
    }
    const bool hug = attrs.size() == 1 && !a_.forced(attrs[0]);
    if (hug) {
        return a_.group(a_.concat({lt_doc, name_doc, a_.text(" "), attrs[0],
                                   a_.text(self_closing ? " />" : ">"), close_tail}));
    }
    std::vector<DocId> body;
    for (const DocId attr : attrs) {
        body.push_back(a_.line());
        body.push_back(attr);
    }
    const DocId closing = self_closing ? a_.concat({a_.line(), a_.text("/>")})
                                       : a_.concat({a_.softline(), a_.text(">")});
    return a_.group(
        a_.concat({lt_doc, name_doc, a_.indent(a_.concat(std::move(body))), closing, close_tail}));
}

namespace {

// One JsxText token: leading and trailing whitespace is kept only when it holds no newline (a
// newline there is dropped by JSX), and the words in between are filled to the line width.
bool text_part(DocArena& a, std::string_view raw, Part& part) {
    std::size_t b = 0;
    std::size_t e = raw.size();
    while (b < e && is_space(raw[b])) {
        ++b;
    }
    while (e > b && is_space(raw[e - 1])) {
        --e;
    }
    const std::string lead(raw.substr(0, b));
    const std::string trail(raw.substr(e));
    const bool lead_newline = lead.find('\n') != std::string::npos;
    const bool trail_newline = trail.find('\n') != std::string::npos;
    if (b == e) {
        if (lead_newline) {
            return false;
        }
        part.glue_only = true;
        part.doc = a.text(lead);
        return true;
    }
    part.text = true;
    if (!lead_newline) {
        part.lead_glue = lead;
    }
    if (!trail_newline) {
        part.trail_glue = trail;
    }
    std::vector<DocId> pieces;
    std::string word;
    std::size_t i = b;
    while (i < e) {
        if (!is_space(raw[i])) {
            word.push_back(raw[i++]);
            continue;
        }
        std::size_t k = i;
        while (k < e && is_space(raw[k])) {
            ++k;
        }
        const std::string_view run = raw.substr(i, k - i);
        if (run.find('\n') != std::string_view::npos || run == " ") {
            pieces.push_back(a.text(std::move(word)));
            pieces.push_back(a.line());
            word.clear();
        } else {
            word.append(run); // a run of spaces on one line is text, not a separator
        }
        i = k;
    }
    pieces.push_back(a.text(std::move(word)));
    part.doc = pieces.size() == 1 ? pieces[0] : a.fill(std::move(pieces));
    return true;
}

} // namespace

std::size_t Builder::jsx_element(std::size_t index, DocId& out, bool& forced_break) {
    std::size_t cursor = 0;
    std::string name;
    bool self_closing = false;
    const DocId open = jsx_open_tag(index, cursor, name, self_closing);
    const std::size_t extent = jsx_extent(index);
    if (self_closing || !error_.empty()) {
        out = open;
        forced_break = false;
        return extent;
    }
    std::vector<Part> parts;
    DocId closing = a_.empty();
    std::size_t nodes = 0;
    bool has_text = false;
    bool pending_blank = false;
    while (error_.empty() && cursor < s_.tokens.size()) {
        const FTok& t = s_.tokens[cursor];
        if (t.kind == FKind::JsxText) {
            Part part;
            const std::string_view raw = t.text;
            auto blank_in = [](std::string_view ws) {
                return std::count(ws.begin(), ws.end(), '\n') >= 2;
            };
            std::size_t b = 0;
            std::size_t e = raw.size();
            while (b < e && is_space(raw[b])) {
                ++b;
            }
            while (e > b && is_space(raw[e - 1])) {
                --e;
            }
            if (text_part(a_, raw, part)) {
                has_text = has_text || part.text;
                part.blank_before = pending_blank || blank_in(raw.substr(0, b));
                pending_blank = b < e && blank_in(raw.substr(e));
                parts.push_back(std::move(part));
            } else {
                pending_blank = pending_blank || blank_in(raw);
            }
            ++cursor;
        } else if (t.is("{")) {
            Part part;
            part.node = true;
            part.blank_before = pending_blank;
            pending_blank = false;
            part.doc = jsx_container(cursor);
            ++nodes;
            parts.push_back(std::move(part));
            cursor = static_cast<std::size_t>(t.match) + 1;
        } else if (t.jsx_lt && s_.tokens[cursor + 1].is("/>")) {
            closing = a_.concat({a_.text("</>"), trailing_docs(cursor + 1, true)});
            cursor += 2;
            break;
        } else if (t.jsx_lt && s_.tokens[cursor + 1].is("/")) {
            std::size_t after = cursor + 2;
            std::string close_name;
            std::size_t last = after;
            if (!s_.tokens[after].is(">")) {
                close_name = collect_name(after, last);
            }
            if (!s_.tokens[after].is(">")) {
                fail("malformed closing tag", t.begin);
                return extent;
            }
            closing = a_.concat({a_.text("</" + close_name + ">"), trailing_docs(after, true)});
            cursor = after + 1;
            break;
        } else if (t.jsx_lt) {
            Part part;
            part.node = true;
            part.blank_before = pending_blank;
            pending_blank = false;
            bool inner_forced = false;
            const std::size_t next = jsx_element(cursor, part.doc, inner_forced);
            ++nodes;
            parts.push_back(std::move(part));
            cursor = next;
        } else {
            fail("unexpected token in JSX children", t.begin);
            return extent;
        }
    }
    if (parts.empty()) {
        out = a_.concat({open, closing});
        forced_break = false;
        return extent;
    }
    // Several nodes and no prose read better one per line, whatever the width.
    forced_break = nodes >= 2 && !has_text;
    std::vector<DocId> kids;
    for (std::size_t k = 0; k < parts.size(); ++k) {
        const Part& part = parts[k];
        if (k > 0) {
            const Part& prev = parts[k - 1];
            if (prev.trail_glue.empty() && part.lead_glue.empty() && !prev.glue_only &&
                !part.glue_only) {
                kids.push_back(a_.softline());
                if (part.blank_before) {
                    kids.push_back(a_.if_break(a_.hardline(), a_.empty()));
                }
            }
        }
        if (!part.lead_glue.empty()) {
            kids.push_back(a_.text(part.lead_glue));
        }
        kids.push_back(part.doc);
        if (!part.trail_glue.empty()) {
            kids.push_back(a_.text(part.trail_glue));
        }
    }
    const Part& first = parts.front();
    const Part& last = parts.back();
    const bool lead_break = first.lead_glue.empty() && !first.glue_only;
    const bool trail_break = last.trail_glue.empty() && !last.glue_only;
    std::vector<DocId> inner;
    if (lead_break) {
        inner.push_back(forced_break ? a_.hardline() : a_.softline());
    }
    inner.push_back(a_.concat(std::move(kids)));
    out = a_.group(a_.concat({open, a_.indent(a_.concat(std::move(inner))),
                              trail_break ? a_.softline() : a_.empty(), closing}));
    return extent;
}

} // namespace tessera::compiler::format
