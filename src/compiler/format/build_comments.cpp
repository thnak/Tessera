// SPDX-License-Identifier: MIT
//
// Comment attachment and token text for the formatter builder (ADR-061, property 3). The rules are
// the whole contract: an own-line comment leads the next token, a same-line comment trails the
// previous one, a comment in an empty construct is dangling. A comment that no emission path
// places is reported by format.cpp as a refusal naming it; nothing is dropped or guessed.

#include "builder.hpp"

#include <utility>

namespace tessera::compiler::format {
namespace {

std::string trim_right(std::string_view text) {
    std::size_t end = text.size();
    while (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
        --end;
    }
    return std::string(text.substr(0, end));
}

std::vector<std::string_view> split_lines(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t begin = 0;
    for (;;) {
        const std::size_t end = text.find('\n', begin);
        if (end == std::string_view::npos) {
            lines.push_back(text.substr(begin));
            return lines;
        }
        lines.push_back(text.substr(begin, end - begin));
        begin = end + 1;
    }
}

} // namespace

void Builder::fail(std::string message, std::uint32_t offset) {
    if (error_.empty()) {
        error_ = std::move(message);
        error_offset_ = offset;
    }
}

void Builder::fail_comment(std::uint32_t id, std::string message) {
    if (error_.empty()) {
        error_ = std::move(message);
        error_offset_ = s_.comments[id].begin;
        error_is_comment_ = true;
    }
}

DocId Builder::comment_doc(std::uint32_t id) {
    FComment& comment = s_.comments[id];
    comment.placed = true;
    if (!comment.block) {
        return a_.text(trim_right(comment.text));
    }
    const std::vector<std::string_view> lines = split_lines(comment.text);
    if (lines.size() == 1) {
        return a_.text(std::string(comment.text));
    }
    // A JSDoc-shaped block (every continuation line starts with `*`) is re-indented to the
    // surrounding code; any other multi-line block is kept byte for byte.
    bool jsdoc = true;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        std::size_t k = 0;
        while (k < lines[i].size() && (lines[i][k] == ' ' || lines[i][k] == '\t')) {
            ++k;
        }
        if (k >= lines[i].size() || lines[i][k] != '*') {
            jsdoc = false;
            break;
        }
    }
    if (!jsdoc) {
        return a_.text(std::string(comment.text));
    }
    std::vector<DocId> parts{a_.text(trim_right(lines[0]))};
    for (std::size_t i = 1; i < lines.size(); ++i) {
        std::size_t k = 0;
        while (lines[i][k] == ' ' || lines[i][k] == '\t') {
            ++k;
        }
        parts.push_back(a_.hardline());
        parts.push_back(a_.text(" " + trim_right(lines[i].substr(k))));
    }
    return a_.concat(std::move(parts));
}

DocId Builder::trailing_docs(std::size_t index, bool space_before_block) {
    const FTok& token = s_.tokens[index];
    if (token.trailing.empty()) {
        return a_.empty();
    }
    std::vector<DocId> parts;
    for (const std::uint32_t id : token.trailing) {
        if (s_.comments[id].block) {
            if (space_before_block) {
                parts.push_back(a_.text(" "));
            }
            parts.push_back(comment_doc(id));
        } else {
            s_.comments[id].placed = true;
            parts.push_back(a_.line_suffix(" " + trim_right(s_.comments[id].text)));
        }
    }
    return a_.concat(std::move(parts));
}

bool Builder::first_blank(std::size_t index) const {
    const FTok& token = s_.tokens[index];
    return token.leading.empty() ? token.blank_before : token.leading.front().blank_before;
}

DocId Builder::lead(std::size_t index) {
    FTok& token = s_.tokens[index];
    if (token.leading.empty()) {
        return a_.empty();
    }
    token.leading_used = true;
    std::vector<DocId> parts;
    for (std::size_t i = 0; i < token.leading.size(); ++i) {
        if (i > 0 && token.leading[i].blank_before) {
            parts.push_back(a_.hardline());
        }
        parts.push_back(comment_doc(token.leading[i].id));
        parts.push_back(a_.hardline());
    }
    if (token.blank_before) {
        parts.push_back(a_.hardline());
    }
    return a_.concat(std::move(parts));
}

// Own-line comments before a closing bracket: each on its own line after the last item.
DocId Builder::lead_close(std::size_t closer) {
    FTok& token = s_.tokens[closer];
    if (token.leading.empty()) {
        return a_.empty();
    }
    token.leading_used = true;
    std::vector<DocId> parts;
    for (std::size_t i = 0; i < token.leading.size(); ++i) {
        if (i > 0 && token.leading[i].blank_before) {
            parts.push_back(a_.hardline());
        }
        parts.push_back(a_.hardline());
        parts.push_back(comment_doc(token.leading[i].id));
    }
    return a_.concat(std::move(parts));
}

bool Builder::has_opener_comments(std::size_t opener) const {
    return !s_.tokens[opener].trailing.empty();
}

// Comments right after an opening bracket. A line comment is held to the end of the line, so it
// goes right after the bracket (`lines`); a block comment stays inline before the first item.
std::vector<DocId> Builder::opener_comments(std::size_t opener, bool lines) {
    std::vector<DocId> parts;
    for (const std::uint32_t id : s_.tokens[opener].trailing) {
        if (s_.comments[id].block && !lines) {
            parts.push_back(comment_doc(id));
            parts.push_back(a_.text(" "));
        } else if (!s_.comments[id].block && lines) {
            s_.comments[id].placed = true;
            parts.push_back(a_.line_suffix(" " + trim_right(s_.comments[id].text)));
        }
    }
    return parts;
}

std::string Builder::token_text(std::size_t index) const {
    const FTok& token = s_.tokens[index];
    if (token.kind == FKind::String && token.text.size() >= 2 && token.text.front() == '\'') {
        const std::string_view inner = token.text.substr(1, token.text.size() - 2);
        if (inner.find('"') == std::string_view::npos &&
            inner.find('\\') == std::string_view::npos) {
            return "\"" + std::string(inner) + "\"";
        }
    }
    return std::string(token.text);
}

DocId Builder::tok(std::size_t index) {
    return a_.concat({a_.text(token_text(index)), trailing_docs(index, true)});
}

bool Builder::gap_has_space(std::size_t before, std::size_t after) const {
    return s_.tokens[after].begin > s_.tokens[before].end;
}

} // namespace tessera::compiler::format
