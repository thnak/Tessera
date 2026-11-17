// SPDX-License-Identifier: MIT

#include "module_rewrite.hpp"

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::compiler::css_module::detail {
namespace {

bool is_grouping_at_rule(std::string_view name) {
    // The blocks whose contents are rules, so a class inside them is still a selector. Everything
    // else (`@font-face`, `@property`, `@page`, ...) holds declarations only and is skipped whole.
    return name == "media" || name == "supports" || name == "layer" || name == "container" ||
           name == "scope" || name == "document" || name == "starting-style";
}

bool is_keyframes_at_rule(std::string_view name) {
    return name == "keyframes" || name == "-webkit-keyframes";
}

bool is_animation_property(std::string_view name) {
    return name == "animation" || name == "animation-name" || name == "-webkit-animation" ||
           name == "-webkit-animation-name";
}

} // namespace

std::expected<void, ModuleScanError> Scanner::scan_rules(bool nested) {
    while (true) {
        pos_ = skip_trivia(pos_);
        const ui::style::Token& token = token_at(pos_);
        if (token.type == ui::style::TokenType::Eof) {
            if (nested) {
                return std::unexpected(
                    malformed("unbalanced braces: a block is never closed", token.offset));
            }
            return {};
        }
        if (token.type == ui::style::TokenType::RightBrace) {
            if (!nested) {
                return std::unexpected(
                    malformed("unbalanced braces: unexpected `}`", token.offset));
            }
            ++pos_;
            return {};
        }
        if (token.type == ui::style::TokenType::AtKeyword) {
            const auto rule = scan_at_rule();
            if (!rule) {
                return rule;
            }
            continue;
        }
        const auto rule = scan_qualified_rule();
        if (!rule) {
            return rule;
        }
    }
}

std::expected<void, ModuleScanError> Scanner::scan_qualified_rule() {
    const std::size_t begin = pos_;
    ui::style::TokenType stop = ui::style::TokenType::Eof;
    const std::size_t stop_index = find_block_end(begin, stop);
    if (stop != ui::style::TokenType::LeftBrace) {
        return std::unexpected(
            malformed("truncated rule: a selector has no `{` block", token_at(begin).offset));
    }
    const auto selector = rewrite_selector(begin, stop_index);
    if (!selector) {
        return selector;
    }
    pos_ = stop_index + 1;
    return scan_declarations();
}

std::expected<void, ModuleScanError> Scanner::scan_declarations() {
    while (true) {
        pos_ = skip_trivia(pos_);
        const ui::style::Token& token = token_at(pos_);
        if (token.type == ui::style::TokenType::Eof) {
            return std::unexpected(
                malformed("unbalanced braces: a declaration block is never closed", token.offset));
        }
        if (token.type == ui::style::TokenType::RightBrace) {
            ++pos_;
            return {};
        }
        if (token.type == ui::style::TokenType::Semicolon) {
            ++pos_;
            continue;
        }
        if (token.type == ui::style::TokenType::AtKeyword) {
            const auto rule = scan_at_rule();
            if (!rule) {
                return rule;
            }
            continue;
        }

        const std::size_t begin = pos_;
        ui::style::TokenType stop = ui::style::TokenType::Eof;
        const std::size_t stop_index = find_block_end(begin, stop);
        if (stop == ui::style::TokenType::LeftBrace) {
            // CSS nesting: a rule inside a declaration block is still a selector.
            const auto selector = rewrite_selector(begin, stop_index);
            if (!selector) {
                return selector;
            }
            pos_ = stop_index + 1;
            const auto nested = scan_declarations();
            if (!nested) {
                return nested;
            }
            continue;
        }
        if (stop == ui::style::TokenType::Eof) {
            return std::unexpected(
                malformed("truncated declaration: no `;` or `}`", token_at(begin).offset));
        }
        rewrite_declaration(begin, stop_index);
        pos_ = stop_index + 1;
        if (stop == ui::style::TokenType::RightBrace) {
            return {};
        }
    }
}

std::expected<void, ModuleScanError> Scanner::scan_keyframes_body() {
    while (true) {
        pos_ = skip_trivia(pos_);
        const ui::style::Token& token = token_at(pos_);
        if (token.type == ui::style::TokenType::Eof) {
            return std::unexpected(
                malformed("unbalanced braces: `@keyframes` is never closed", token.offset));
        }
        if (token.type == ui::style::TokenType::RightBrace) {
            ++pos_;
            return {};
        }
        // The prelude here is `from`, `to` or a percentage: a keyframe selector, never a class.
        const std::size_t begin = pos_;
        ui::style::TokenType stop = ui::style::TokenType::Eof;
        const std::size_t stop_index = find_block_end(begin, stop);
        if (stop != ui::style::TokenType::LeftBrace) {
            return std::unexpected(
                malformed("truncated `@keyframes` step: no `{` block", token_at(begin).offset));
        }
        pos_ = stop_index + 1;
        const auto declarations = scan_declarations();
        if (!declarations) {
            return declarations;
        }
    }
}

std::expected<void, ModuleScanError> Scanner::skip_block() {
    // `pos_` is on the `{`. Nothing inside is a selector, so the block is balanced and dropped.
    std::size_t depth = 0;
    while (pos_ < tokens_.size()) {
        const ui::style::Token& token = tokens_[pos_];
        if (token.type == ui::style::TokenType::Eof) {
            return std::unexpected(
                malformed("unbalanced braces: a block is never closed", token.offset));
        }
        if (token.type == ui::style::TokenType::LeftBrace) {
            ++depth;
        } else if (token.type == ui::style::TokenType::RightBrace) {
            --depth;
            if (depth == 0) {
                ++pos_;
                return {};
            }
        }
        ++pos_;
    }
    return std::unexpected(malformed("unbalanced braces: a block is never closed", text_.size()));
}

std::expected<void, ModuleScanError> Scanner::scan_at_rule() {
    const ui::style::Token& at = token_at(pos_);
    const std::string name = to_lower(at.value);
    const std::size_t at_offset = at.offset;
    ++pos_;

    if (is_keyframes_at_rule(name)) {
        const std::size_t name_index = skip_trivia(pos_);
        if (token_at(name_index).type != ui::style::TokenType::Ident) {
            return std::unexpected(
                malformed("`@keyframes` has no name", token_at(name_index).offset));
        }
        rename_keyframes(tokens_[name_index]);
        pos_ = name_index + 1;
        ui::style::TokenType stop = ui::style::TokenType::Eof;
        const std::size_t stop_index = find_block_end(pos_, stop);
        if (stop != ui::style::TokenType::LeftBrace) {
            return std::unexpected(malformed("`@keyframes` has no `{` block", at_offset));
        }
        pos_ = stop_index + 1;
        return scan_keyframes_body();
    }

    ui::style::TokenType stop = ui::style::TokenType::Eof;
    const std::size_t stop_index = find_block_end(pos_, stop);
    if (stop == ui::style::TokenType::Semicolon) {
        // A statement at-rule (`@import`, `@layer a, b;`, `@charset`): nothing to rewrite.
        pos_ = stop_index + 1;
        return {};
    }
    if (stop != ui::style::TokenType::LeftBrace) {
        return std::unexpected(malformed("truncated at-rule: no `{` block", at_offset));
    }
    if (is_grouping_at_rule(name)) {
        pos_ = stop_index + 1;
        return scan_rules(true);
    }
    pos_ = stop_index;
    return skip_block();
}

} // namespace tessera::compiler::css_module::detail
