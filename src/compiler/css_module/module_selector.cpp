// SPDX-License-Identifier: MIT
//
// The span rewriters: which bytes of a selector or a declaration become a scoped name. They are
// separate from `module_walk.cpp` (which decides *where* the spans are) because the two answer
// different questions and the walk alone was over the file-size soft limit.

#include "module_rewrite.hpp"

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

namespace tessera::compiler::css_module::detail {
namespace {

bool is_animation_property(std::string_view name) {
    return name == "animation" || name == "animation-name" || name == "-webkit-animation" ||
           name == "-webkit-animation-name";
}

} // namespace

std::expected<void, ModuleScanError> Scanner::rewrite_selector(std::size_t begin, std::size_t end) {
    std::size_t bracket_depth = 0;
    for (std::size_t i = begin; i < end; ++i) {
        const ui::style::Token& token = tokens_[i];
        if (token.type == ui::style::TokenType::LeftBracket) {
            ++bracket_depth;
            continue;
        }
        if (token.type == ui::style::TokenType::RightBracket) {
            if (bracket_depth > 0) {
                --bracket_depth;
            }
            continue;
        }
        // An attribute selector's contents are never class selectors.
        if (bracket_depth > 0) {
            continue;
        }
        if (token.type == ui::style::TokenType::Colon) {
            const std::size_t next = i + 1;
            if (next >= end) {
                continue;
            }
            const ui::style::Token& after = tokens_[next];
            if (after.type == ui::style::TokenType::Function &&
                equals_ignore_case(after.value, "global")) {
                const auto close = find_matching_paren(next, end);
                if (!close) {
                    return std::unexpected(close.error());
                }
                // Drop the `:global(` wrapper and its `)`, leaving the contents untouched.
                edits_.push_back(
                    Edit{token.offset, after.offset + after.text.size() - token.offset, {}});
                edits_.push_back(Edit{tokens_[*close].offset, tokens_[*close].text.size(), {}});
                i = *close;
                continue;
            }
            if (after.type == ui::style::TokenType::Ident &&
                equals_ignore_case(after.value, "global")) {
                return std::unexpected(ModuleScanError{
                    std::string(kGlobalBlockCode),
                    ":global { } block form is not supported; use :global(.class)", token.offset});
            }
            continue;
        }
        if (token.type == ui::style::TokenType::Delim && token.text == ".") {
            const std::size_t next = i + 1;
            if (next < end && tokens_[next].type == ui::style::TokenType::Ident) {
                rename_class(tokens_[next]);
                ++i;
            }
        }
    }
    return {};
}

void Scanner::rewrite_declaration(std::size_t begin, std::size_t end) {
    std::size_t colon = kNoIndex;
    std::size_t paren_depth = 0;
    for (std::size_t i = begin; i < end; ++i) {
        const ui::style::TokenType type = tokens_[i].type;
        if (type == ui::style::TokenType::Function || type == ui::style::TokenType::LeftParen) {
            ++paren_depth;
            continue;
        }
        if (type == ui::style::TokenType::RightParen) {
            if (paren_depth > 0) {
                --paren_depth;
            }
            continue;
        }
        if (paren_depth == 0 && type == ui::style::TokenType::Colon) {
            colon = i;
            break;
        }
    }
    if (colon == kNoIndex) {
        return;
    }
    const std::size_t name_index = skip_trivia(begin);
    if (name_index >= colon || tokens_[name_index].type != ui::style::TokenType::Ident) {
        return;
    }
    if (!is_animation_property(to_lower(tokens_[name_index].value))) {
        return;
    }
    rewrite_animation_value(colon + 1, end);
}

void Scanner::rewrite_animation_value(std::size_t begin, std::size_t end) {
    for (std::size_t i = begin; i < end; ++i) {
        if (tokens_[i].type != ui::style::TokenType::Ident) {
            continue;
        }
        // Only a name this file declares is scoped; `spin` from another module stays as written.
        if (!is_keyframe_name(tokens_[i].value)) {
            continue;
        }
        rename_keyframes(tokens_[i]);
    }
}

} // namespace tessera::compiler::css_module::detail
