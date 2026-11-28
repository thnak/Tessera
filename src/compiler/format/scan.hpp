// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/lexer.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The formatter works on the token stream, not on the AST: the AST drops parentheses, desugars
// `useMemo`, expands intersections and skips helper bodies, so printing from it could not promise
// that the output means what the input meant (ADR-061 property 2). This scanner drives the
// parser's own lexer through the same modes the parser would use and records every token with its
// comments, so nothing the author wrote can be lost.
namespace tessera::compiler::format {

enum class FKind : std::uint8_t {
    Word, // identifier or keyword
    Number,
    String,
    Punct,
    Atom,    // a whole template literal, substitutions included, printed verbatim
    JsxText, // raw text between tags
    Eof,
};

struct LeadComment {
    std::uint32_t id = 0;
    bool blank_before = false; // a blank line between the previous thing and this comment
};

struct FTok {
    FKind kind = FKind::Eof;
    jsx::LexMode mode = jsx::LexMode::Code;
    bool keyword = false;
    bool jsx_lt = false;  // the `<` that opens a JSX element or fragment
    bool generic = false; // a `<` or `>` that brackets type arguments, not a comparison
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    std::string_view text;
    std::int32_t match = -1;             // bracket partner, as a token index
    std::vector<LeadComment> leading;    // own-line comments that lead this token
    std::vector<std::uint32_t> trailing; // same-line comments after the previous token
    bool blank_before = false;           // blank line between the last comment (or token) and this
    bool leading_used = false;

    [[nodiscard]] bool is(std::string_view punct) const {
        return kind == FKind::Punct && text == punct;
    }
    [[nodiscard]] bool is_word(std::string_view word) const {
        return kind == FKind::Word && text == word;
    }
};

struct FComment {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    bool block = false;
    std::string_view text; // including the delimiters
    bool placed = false;
};

struct ScanResult {
    std::vector<FTok> tokens; // the last one is Eof
    std::vector<FComment> comments;
    std::string error; // non-empty when the file could not be scanned
    std::uint32_t error_offset = 0;
    bool error_is_comment = false;
};

// `source` must already parse without errors; the scanner only reports what it cannot place.
ScanResult scan(std::string_view source);

} // namespace tessera::compiler::format
