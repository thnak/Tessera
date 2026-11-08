// SPDX-License-Identifier: MIT

#include "tessera/ui/style/parser.hpp"
#include "tessera/ui/style/tokenizer.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace tessera::ui::style {
namespace {

bool is_trivia(TokenType type) {
    return type == TokenType::Whitespace || type == TokenType::Comment;
}

void append_text(std::string& output, const Token& token) {
    if (token.type == TokenType::Comment) {
        if (!output.empty() && output.back() != ' ') {
            output.push_back(' ');
        }
        return;
    }
    output.append(token.text.data(), token.text.size());
}

std::string trim(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(),
                                        [](unsigned char c) { return std::isspace(c) != 0; });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
                          return std::isspace(c) != 0;
                      }).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool strip_important(std::string& value) {
    const std::string trimmed = trim(value);
    const std::string lowered = lower(trimmed);
    constexpr std::string_view kImportant = "important";
    if (lowered.size() < kImportant.size() ||
        lowered.compare(lowered.size() - kImportant.size(), kImportant.size(), kImportant) != 0) {
        return false;
    }

    const std::size_t important_start = lowered.size() - kImportant.size();
    const std::size_t bang = lowered.rfind('!', important_start);
    if (bang == std::string::npos) {
        return false;
    }
    for (std::size_t i = bang + 1; i < important_start; ++i) {
        if (std::isspace(static_cast<unsigned char>(lowered[i])) == 0) {
            return false;
        }
    }

    const std::size_t original_bang = trimmed.rfind('!');
    if (original_bang == std::string::npos) {
        return false;
    }
    value = trim(trimmed.substr(0, original_bang));
    return true;
}

class Parser {
public:
    explicit Parser(std::vector<Token> tokens)
        : tokens_(std::move(tokens)) {}

    StyleSheet parse() {
        StyleSheet sheet;
        while (!at_end()) {
            skip_trivia();
            if (at_end()) {
                break;
            }
            if (peek().type == TokenType::AtKeyword) {
                sheet.at_rules.push_back(parse_at_rule());
            } else if (peek().type == TokenType::RightBrace ||
                       peek().type == TokenType::Semicolon) {
                take();
            } else {
                Rule rule = parse_rule();
                if (!rule.selector.empty()) {
                    sheet.rules.push_back(std::move(rule));
                }
            }
        }
        return sheet;
    }

private:
    [[nodiscard]] bool at_end() const {
        return index_ >= tokens_.size() || tokens_[index_].type == TokenType::Eof;
    }
    [[nodiscard]] const Token& peek() const {
        return tokens_[index_];
    }
    Token take() {
        return tokens_[index_++];
    }
    void skip_trivia() {
        while (!at_end() && is_trivia(peek().type)) {
            ++index_;
        }
    }

    AtRule parse_at_rule() {
        const Token at = take();
        AtRule rule;
        rule.name = std::string(at.value);
        rule.offset = at.offset;

        int depth = 0;
        while (!at_end()) {
            const Token token = peek();
            if (depth == 0 &&
                (token.type == TokenType::LeftBrace || token.type == TokenType::Semicolon)) {
                break;
            }
            take();
            if (token.type == TokenType::Function || token.type == TokenType::LeftParen ||
                token.type == TokenType::LeftBracket) {
                ++depth;
            } else if (token.type == TokenType::RightParen ||
                       token.type == TokenType::RightBracket) {
                --depth;
            }
            append_text(rule.prelude, token);
        }
        rule.prelude = trim(rule.prelude);

        if (!at_end() && peek().type == TokenType::Semicolon) {
            take();
            return rule;
        }
        if (at_end() || peek().type != TokenType::LeftBrace) {
            return rule;
        }

        take();
        rule.has_block = true;
        int braces = 1;
        while (!at_end()) {
            const Token token = take();
            if (token.type == TokenType::LeftBrace) {
                ++braces;
            } else if (token.type == TokenType::RightBrace) {
                --braces;
                if (braces == 0) {
                    break;
                }
            }
            append_text(rule.block, token);
        }
        return rule;
    }

    Rule parse_rule() {
        Rule rule;
        rule.offset = peek().offset;

        int depth = 0;
        bool found_block = false;
        while (!at_end()) {
            const Token token = peek();
            if (depth == 0 && token.type == TokenType::LeftBrace) {
                take();
                found_block = true;
                break;
            }
            if (depth == 0 &&
                (token.type == TokenType::Semicolon || token.type == TokenType::RightBrace)) {
                break;
            }
            take();
            if (token.type == TokenType::Function || token.type == TokenType::LeftParen ||
                token.type == TokenType::LeftBracket) {
                ++depth;
            } else if (token.type == TokenType::RightParen ||
                       token.type == TokenType::RightBracket) {
                --depth;
            }
            append_text(rule.selector, token);
        }
        rule.selector = trim(rule.selector);

        if (!found_block) {
            return rule;
        }
        rule.declarations = parse_declarations();
        return rule;
    }

    std::vector<Declaration> parse_declarations() {
        std::vector<Declaration> declarations;
        while (!at_end()) {
            skip_trivia();
            if (at_end()) {
                break;
            }
            if (peek().type == TokenType::RightBrace) {
                take();
                break;
            }
            if (peek().type == TokenType::Semicolon) {
                take();
                continue;
            }
            if (peek().type == TokenType::AtKeyword) {
                skip_statement_or_block();
                continue;
            }
            if (peek().type != TokenType::Ident) {
                skip_to_declaration_end();
                continue;
            }

            Declaration declaration;
            declaration.offset = peek().offset;
            declaration.property = std::string(take().value);
            skip_trivia();
            if (at_end() || peek().type != TokenType::Colon) {
                skip_to_declaration_end();
                continue;
            }
            take();
            declaration.value = collect_value();
            declaration.important = strip_important(declaration.value);
            if (!declaration.value.empty()) {
                declarations.push_back(std::move(declaration));
            }
        }
        return declarations;
    }

    std::string collect_value() {
        std::string value;
        int depth = 0;
        while (!at_end()) {
            const Token& token = peek();
            if (depth == 0 &&
                (token.type == TokenType::Semicolon || token.type == TokenType::RightBrace)) {
                break;
            }
            const Token current = take();
            if (current.type == TokenType::Function || current.type == TokenType::LeftParen ||
                current.type == TokenType::LeftBracket) {
                ++depth;
            } else if (current.type == TokenType::RightParen ||
                       current.type == TokenType::RightBracket) {
                --depth;
            }
            append_text(value, current);
        }
        return trim(value);
    }

    void skip_to_declaration_end() {
        int depth = 0;
        while (!at_end()) {
            const Token token = peek();
            if (depth == 0 &&
                (token.type == TokenType::Semicolon || token.type == TokenType::RightBrace)) {
                return;
            }
            take();
            if (token.type == TokenType::Function || token.type == TokenType::LeftParen ||
                token.type == TokenType::LeftBracket || token.type == TokenType::LeftBrace) {
                ++depth;
            } else if (token.type == TokenType::RightParen ||
                       token.type == TokenType::RightBracket ||
                       token.type == TokenType::RightBrace) {
                --depth;
            }
        }
    }

    void skip_statement_or_block() {
        int depth = 0;
        while (!at_end()) {
            const Token token = take();
            if (depth == 0 && token.type == TokenType::Semicolon) {
                return;
            }
            if (token.type == TokenType::Function || token.type == TokenType::LeftParen ||
                token.type == TokenType::LeftBracket || token.type == TokenType::LeftBrace) {
                ++depth;
            } else if (token.type == TokenType::RightParen ||
                       token.type == TokenType::RightBracket ||
                       token.type == TokenType::RightBrace) {
                if (depth == 0) {
                    return;
                }
                --depth;
                if (depth == 0) {
                    return;
                }
            }
        }
    }

    std::vector<Token> tokens_;
    std::size_t index_ = 0;
};

} // namespace

StyleSheet parse_stylesheet(std::string_view source) {
    Tokenizer tokenizer(source);
    Parser parser(tokenizer.tokenize());
    return parser.parse();
}

} // namespace tessera::ui::style
