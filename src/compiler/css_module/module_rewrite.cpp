// SPDX-License-Identifier: MIT

#include "module_rewrite.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::css_module::detail {
namespace {

bool is_trivia(ui::style::TokenType type) {
    return type == ui::style::TokenType::Whitespace || type == ui::style::TokenType::Comment ||
           type == ui::style::TokenType::Cdo || type == ui::style::TokenType::Cdc;
}

char lower_char(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

} // namespace

std::string to_lower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(lower_char(c));
    }
    return out;
}

bool equals_ignore_case(std::string_view text, std::string_view expected) {
    if (text.size() != expected.size()) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (lower_char(text[i]) != expected[i]) {
            return false;
        }
    }
    return true;
}

Scanner::Scanner(std::string_view text, std::string_view hash, std::vector<ui::style::Token> tokens)
    : text_(text),
      hash_(hash),
      tokens_(std::move(tokens)) {}

std::expected<RewriteResult, ModuleScanError> Scanner::run() {
    const auto pre = pre_scan();
    if (!pre) {
        return std::unexpected(pre.error());
    }

    pos_ = 0;
    const auto walk = scan_rules(false);
    if (!walk) {
        return std::unexpected(walk.error());
    }

    const auto by_local = [](const auto& a, const auto& b) { return a.local < b.local; };
    const auto same_local = [](const auto& a, const auto& b) { return a.local == b.local; };
    std::sort(classes_.begin(), classes_.end(), by_local);
    classes_.erase(std::unique(classes_.begin(), classes_.end(), same_local), classes_.end());
    std::sort(keyframes_.begin(), keyframes_.end(), by_local);
    keyframes_.erase(std::unique(keyframes_.begin(), keyframes_.end(), same_local),
                     keyframes_.end());

    std::string rewritten = build_text();
    return RewriteResult{std::move(classes_), std::move(keyframes_), std::move(rewritten)};
}

const ui::style::Token& Scanner::token_at(std::size_t index) const {
    // `tokenize()` always appends the Eof token, so the vector is never empty.
    if (index >= tokens_.size()) {
        return tokens_.back();
    }
    return tokens_[index];
}

std::size_t Scanner::skip_trivia(std::size_t index) const {
    while (index < tokens_.size() && is_trivia(tokens_[index].type)) {
        ++index;
    }
    return index;
}

std::size_t Scanner::find_block_end(std::size_t index, ui::style::TokenType& stop) const {
    std::size_t parens = 0;
    std::size_t brackets = 0;
    for (std::size_t i = index; i < tokens_.size(); ++i) {
        const ui::style::TokenType type = tokens_[i].type;
        if (type == ui::style::TokenType::Eof) {
            stop = type;
            return i;
        }
        if (type == ui::style::TokenType::Function || type == ui::style::TokenType::LeftParen) {
            ++parens;
            continue;
        }
        if (type == ui::style::TokenType::RightParen) {
            if (parens > 0) {
                --parens;
            }
            continue;
        }
        if (type == ui::style::TokenType::LeftBracket) {
            ++brackets;
            continue;
        }
        if (type == ui::style::TokenType::RightBracket) {
            if (brackets > 0) {
                --brackets;
            }
            continue;
        }
        if (parens > 0 || brackets > 0) {
            continue;
        }
        if (type == ui::style::TokenType::Semicolon || type == ui::style::TokenType::LeftBrace ||
            type == ui::style::TokenType::RightBrace) {
            stop = type;
            return i;
        }
    }
    stop = ui::style::TokenType::Eof;
    return tokens_.size() - 1;
}

std::expected<std::size_t, ModuleScanError> Scanner::find_matching_paren(std::size_t open_index,
                                                                         std::size_t end) const {
    std::size_t depth = 0;
    for (std::size_t i = open_index; i < end; ++i) {
        const ui::style::TokenType type = tokens_[i].type;
        if (type == ui::style::TokenType::Function || type == ui::style::TokenType::LeftParen) {
            ++depth;
            continue;
        }
        if (type != ui::style::TokenType::RightParen) {
            continue;
        }
        if (depth == 0) {
            break;
        }
        --depth;
        if (depth == 0) {
            return i;
        }
    }
    return std::unexpected(ModuleScanError{std::string(kMalformedGlobalCode),
                                           ":global( has no matching `)`",
                                           tokens_[open_index].offset});
}

std::expected<void, ModuleScanError> Scanner::pre_scan() {
    for (std::size_t i = 0; i < tokens_.size(); ++i) {
        const ui::style::Token& token = tokens_[i];
        // An unclosed `:global(` is its own diagnostic, so it is found before the generic brace
        // balance check can turn the same file into a plain malformed error.
        if (token.type == ui::style::TokenType::Colon && i + 1 < tokens_.size() &&
            tokens_[i + 1].type == ui::style::TokenType::Function &&
            equals_ignore_case(tokens_[i + 1].value, "global")) {
            const auto close = find_matching_paren(i + 1, tokens_.size());
            if (!close) {
                return std::unexpected(close.error());
            }
        }
        if (token.type != ui::style::TokenType::AtKeyword) {
            continue;
        }
        const std::string name = to_lower(token.value);
        if (name != "keyframes" && name != "-webkit-keyframes") {
            continue;
        }
        const std::size_t next = skip_trivia(i + 1);
        if (next >= tokens_.size() || tokens_[next].type != ui::style::TokenType::Ident) {
            continue;
        }
        keyframe_names_.push_back(to_lower(tokens_[next].value));
    }
    std::sort(keyframe_names_.begin(), keyframe_names_.end());
    keyframe_names_.erase(std::unique(keyframe_names_.begin(), keyframe_names_.end()),
                          keyframe_names_.end());
    return {};
}

bool Scanner::is_keyframe_name(std::string_view local) const {
    const std::string lowered = to_lower(local);
    return std::binary_search(keyframe_names_.begin(), keyframe_names_.end(), lowered);
}

void Scanner::rename_class(const ui::style::Token& ident) {
    const std::string local(ident.value);
    std::string scoped = scoped_name(local, hash_);
    edits_.push_back(Edit{ident.offset, ident.text.size(), scoped});
    classes_.push_back(ModuleClass{local, std::move(scoped)});
}

void Scanner::rename_keyframes(const ui::style::Token& ident) {
    const std::string local(ident.value);
    std::string scoped = scoped_name(local, hash_);
    edits_.push_back(Edit{ident.offset, ident.text.size(), scoped});
    keyframes_.push_back(ModuleKeyframes{local, std::move(scoped)});
}

std::string Scanner::build_text() const {
    std::vector<Edit> ordered = edits_;
    std::sort(ordered.begin(), ordered.end(),
              [](const Edit& a, const Edit& b) { return a.offset < b.offset; });

    std::string out;
    out.reserve(text_.size() + ordered.size() * 8);
    std::size_t cursor = 0;
    for (const Edit& edit : ordered) {
        // Overlapping edits would mean two constructs claimed the same bytes; the first wins so the
        // output stays a byte-preserving rewrite instead of a corrupted splice.
        if (edit.offset < cursor) {
            continue;
        }
        out.append(text_.substr(cursor, edit.offset - cursor));
        out.append(edit.replacement);
        cursor = edit.offset + edit.length;
    }
    out.append(text_.substr(cursor));
    return out;
}

ModuleScanError Scanner::malformed(std::string message, std::size_t offset) const {
    return ModuleScanError{std::string(kMalformedCode), std::move(message), offset};
}

} // namespace tessera::compiler::css_module::detail
