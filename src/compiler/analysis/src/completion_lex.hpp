// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>

// Byte-level reading of the text around a completion caret (issue #573). Everything here is a pure
// function of the text, so a completion stays a function of the project and the caret.
namespace tessera::compiler::analysis::lex {

// The text up to the caret on its line, ignoring a quote that follows a backslash is not needed
// for the contexts below; an escaped quote is rare enough to cost one wrong "inside a string".
struct Caret {
    std::string_view text;
    std::size_t at = 0;   // byte of the caret
    std::size_t word = 0; // first byte of the identifier prefix before the caret
    std::size_t end = 0;  // end of the identifier at the caret
    std::size_t line = 0; // first byte of the caret's line
    [[nodiscard]] std::string_view before_on_line() const {
        return text.substr(line, at - line);
    }
    [[nodiscard]] std::string_view prefix() const {
        return text.substr(word, at - word);
    }
};

struct OpenTag {
    std::string name;
    std::size_t name_end = 0;
};

bool ident_byte(char c);
bool ident_start(char c);
bool starts_with_icase(std::string_view text, std::string_view prefix);
std::size_t line_start(std::string_view text, std::size_t at);
std::size_t skip_space_back(std::string_view text, std::size_t at);
std::string_view word_before(std::string_view text, std::size_t end);
bool odd_count(std::string_view text, char quote);
// The JSX tag whose attribute list holds byte `at`, or none when an expression container, a
// closed tag or a statement boundary comes first. Best effort on text that is mid-edit.
std::optional<OpenTag> open_tag(std::string_view text, std::size_t at);
// Attribute names already written in the tag head (depth 0, outside strings).
std::set<std::string> written_attributes(std::string_view head);
std::string snake_case(std::string_view name);
// '/' separated path of `target` relative to the directory of `from`, as an import specifier.
std::string specifier(std::string_view from, std::string_view target);

} // namespace tessera::compiler::analysis::lex
