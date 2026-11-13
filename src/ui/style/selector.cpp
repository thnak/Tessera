// SPDX-License-Identifier: MIT

#include "tessera/ui/style/selector.hpp"
#include "tessera/ui/style/tokenizer.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace tessera::ui::style {
namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
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

bool is_compound_start(TokenType type) {
    return type == TokenType::Ident || type == TokenType::Hash || type == TokenType::LeftBracket ||
           type == TokenType::Colon;
}

bool is_delim(const Token& token, char value) {
    return token.type == TokenType::Delim && token.text.size() == 1 && token.text.front() == value;
}

bool is_ident_start(unsigned char c) {
    return std::isalpha(c) != 0 || c == '_' || c == '-';
}

bool is_ident_char(unsigned char c) {
    return std::isalnum(c) != 0 || c == '_' || c == '-';
}

// `::part(<ident>+)` (stylesheet-v1 section 6.3): one or more unquoted identifiers separated by
// whitespace. The parser rejects anything else so a malformed part selector never matches silently.
bool valid_part_argument(std::string_view argument) {
    if (argument.empty()) {
        return false;
    }
    bool at_token_start = true;
    bool any = false;
    for (const char c : argument) {
        const auto byte = static_cast<unsigned char>(c);
        if (std::isspace(byte) != 0) {
            at_token_start = true;
            continue;
        }
        if (at_token_start) {
            if (!is_ident_start(byte)) {
                return false;
            }
            at_token_start = false;
            any = true;
            continue;
        }
        if (!is_ident_char(byte)) {
            return false;
        }
    }
    return any && !at_token_start;
}

class SelectorParser {
public:
    explicit SelectorParser(std::vector<Token> tokens)
        : tokens_(std::move(tokens)) {}

    SelectorParseResult parse() {
        SelectorParseResult result;
        skip_trivia(nullptr);
        if (at_end()) {
            result.ok = false;
            return result;
        }

        while (!at_end()) {
            ComplexSelector complex;
            if (!parse_complex(complex) || complex.compounds.empty()) {
                result.ok = false;
                if (result.error_offset == 0) {
                    result.error_offset = peek().offset;
                }
                skip_to_comma();
            } else {
                result.list.selectors.push_back(std::move(complex));
            }

            skip_trivia(nullptr);
            if (!at_end() && peek().type == TokenType::Comma) {
                take();
                skip_trivia(nullptr);
                if (at_end()) {
                    result.ok = false;
                }
                continue;
            }
            if (!at_end()) {
                result.ok = false;
                if (result.error_offset == 0) {
                    result.error_offset = peek().offset;
                }
                skip_to_comma();
            }
        }
        return result;
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

    void skip_comments() {
        while (!at_end() && peek().type == TokenType::Comment) {
            ++index_;
        }
    }

    void skip_trivia(bool* saw_whitespace) {
        if (saw_whitespace != nullptr) {
            *saw_whitespace = false;
        }
        while (!at_end()) {
            if (peek().type == TokenType::Whitespace) {
                if (saw_whitespace != nullptr) {
                    *saw_whitespace = true;
                }
                ++index_;
            } else if (peek().type == TokenType::Comment) {
                ++index_;
            } else {
                break;
            }
        }
    }

    void skip_to_comma() {
        while (!at_end() && peek().type != TokenType::Comma) {
            ++index_;
        }
    }

    bool parse_complex(ComplexSelector& complex) {
        CompoundSelector first;
        if (!parse_compound(first)) {
            return false;
        }
        complex.compounds.push_back(std::move(first));

        while (true) {
            bool saw_whitespace = false;
            skip_trivia(&saw_whitespace);
            if (at_end() || peek().type == TokenType::Comma) {
                return true;
            }

            Combinator combinator = Combinator::Descendant;
            if (is_delim(peek(), '>') || is_delim(peek(), '+') || is_delim(peek(), '~')) {
                const char value = peek().text.front();
                combinator = value == '>'   ? Combinator::Child
                             : value == '+' ? Combinator::AdjacentSibling
                                            : Combinator::GeneralSibling;
                take();
                skip_trivia(nullptr);
            } else if (!saw_whitespace) {
                return false;
            }

            if (at_end() || peek().type == TokenType::Comma ||
                (!is_compound_start(peek().type) &&
                 !(peek().type == TokenType::Delim &&
                   (is_delim(peek(), '.') || is_delim(peek(), '*'))))) {
                return false;
            }
            // `::part()` may only appear in the subject compound: `.a::part(x) .b` has no meaning
            // once the part element sits inside the component's own subtree.
            if (has_part_pseudo(complex.compounds.back())) {
                return false;
            }

            CompoundSelector next;
            if (!parse_compound(next)) {
                return false;
            }
            complex.combinators.push_back(combinator);
            complex.compounds.push_back(std::move(next));
        }
    }

    bool parse_compound(CompoundSelector& compound) {
        skip_comments();
        bool any = false;
        bool saw_pseudo_element = false;

        if (!at_end() && peek().type == TokenType::Ident) {
            compound.tag = std::string(take().value);
            any = true;
        } else if (!at_end() && is_delim(peek(), '*')) {
            take();
            any = true;
        }

        while (true) {
            skip_comments();
            if (at_end()) {
                break;
            }
            if (is_delim(peek(), '.')) {
                take();
                if (at_end() || peek().type != TokenType::Ident) {
                    return false;
                }
                compound.classes.emplace_back(take().value);
                any = true;
                continue;
            }
            if (peek().type == TokenType::Hash) {
                compound.id = std::string(take().value);
                any = true;
                continue;
            }
            if (peek().type == TokenType::LeftBracket) {
                AttributeSelector attribute;
                if (!parse_attribute(attribute)) {
                    return false;
                }
                compound.attributes.push_back(std::move(attribute));
                any = true;
                continue;
            }
            if (peek().type == TokenType::Colon) {
                PseudoClass pseudo;
                if (!parse_pseudo(pseudo)) {
                    return false;
                }
                // One pseudo-element per compound. `::part()` is the component boundary, so nothing
                // element-shaped may follow it (`::part(x)::part(y)` and `::part(x)::before` are
                // rejected rather than matched as if they were plain pseudo-elements).
                if (pseudo.pseudo_element) {
                    if (saw_pseudo_element) {
                        return false;
                    }
                    saw_pseudo_element = true;
                }
                compound.pseudos.push_back(std::move(pseudo));
                any = true;
                continue;
            }
            break;
        }
        return any;
    }

    bool parse_attribute(AttributeSelector& attribute) {
        take();
        skip_trivia(nullptr);
        if (at_end() || peek().type != TokenType::Ident) {
            return false;
        }
        attribute.name = std::string(take().value);
        skip_trivia(nullptr);

        if (!at_end() && peek().type == TokenType::RightBracket) {
            take();
            return true;
        }

        if (at_end()) {
            return false;
        }
        const Token op = take();
        if (is_delim(op, '=')) {
            attribute.op = AttributeOperator::Equals;
        } else if (op.type == TokenType::IncludeMatch) {
            attribute.op = AttributeOperator::Includes;
        } else if (op.type == TokenType::DashMatch) {
            attribute.op = AttributeOperator::Dash;
        } else if (op.type == TokenType::PrefixMatch) {
            attribute.op = AttributeOperator::Prefix;
        } else if (op.type == TokenType::SuffixMatch) {
            attribute.op = AttributeOperator::Suffix;
        } else if (op.type == TokenType::SubstringMatch) {
            attribute.op = AttributeOperator::Substring;
        } else {
            return false;
        }

        skip_trivia(nullptr);
        if (at_end()) {
            return false;
        }
        const Token value = take();
        if (value.type == TokenType::String) {
            attribute.value = std::string(value.value);
        } else if (value.type == TokenType::Ident || value.type == TokenType::Number ||
                   value.type == TokenType::Dimension) {
            attribute.value = std::string(value.text);
        } else {
            return false;
        }

        skip_trivia(nullptr);
        if (!at_end() && peek().type == TokenType::Ident &&
            lower(std::string(peek().value)) == "i") {
            attribute.case_insensitive = true;
            take();
            skip_trivia(nullptr);
        }
        if (at_end() || peek().type != TokenType::RightBracket) {
            return false;
        }
        take();
        return true;
    }

    bool parse_pseudo(PseudoClass& pseudo) {
        take();
        bool element = false;
        if (!at_end() && peek().type == TokenType::Colon) {
            take();
            element = true;
        }

        if (at_end()) {
            return false;
        }
        if (peek().type == TokenType::Function) {
            const Token function = take();
            pseudo.name = lower(std::string(function.value));
            pseudo.argument = collect_argument();
            pseudo.pseudo_element = element;
            if (element && pseudo.name == "part") {
                // `::part(<ident>+)`: whitespace-separated, unquoted, at least one name.
                if (!valid_part_argument(pseudo.argument)) {
                    return false;
                }
            } else if (!element && (pseudo.name == "not" || pseudo.name == "is" ||
                                    pseudo.name == "where" || pseudo.name == "has")) {
                SelectorParseResult nested = parse_selector_list(pseudo.argument);
                if (nested.ok) {
                    pseudo.nested = std::make_shared<const SelectorList>(std::move(nested.list));
                }
            }
            return true;
        }
        if (peek().type != TokenType::Ident) {
            return false;
        }
        pseudo.name = lower(std::string(take().value));
        pseudo.pseudo_element = element;
        if (element && pseudo.name == "part") {
            return false; // `::part` is functional; a bare `::part` matches nothing
        }
        return true;
    }

    std::string collect_argument() {
        std::string argument;
        int depth = 1;
        while (!at_end()) {
            const Token token = take();
            if (token.type == TokenType::RightParen) {
                --depth;
                if (depth == 0) {
                    break;
                }
            } else if (token.type == TokenType::Function || token.type == TokenType::LeftParen) {
                ++depth;
            } else if (token.type == TokenType::LeftBracket) {
                ++depth;
            } else if (token.type == TokenType::RightBracket) {
                --depth;
            }
            argument.append(token.text.data(), token.text.size());
        }
        return trim(argument);
    }

    std::vector<Token> tokens_;
    std::size_t index_ = 0;
};

} // namespace

SelectorParseResult parse_selector_list(std::string_view text) {
    Tokenizer tokenizer(text);
    SelectorParser parser(tokenizer.tokenize());
    return parser.parse();
}

Specificity specificity(const CompoundSelector& selector) {
    Specificity result;
    if (!selector.tag.empty()) {
        ++result.types;
    }
    if (!selector.id.empty()) {
        ++result.ids;
    }
    result.classes += static_cast<std::uint32_t>(selector.classes.size());
    result.classes += static_cast<std::uint32_t>(selector.attributes.size());

    for (const PseudoClass& pseudo : selector.pseudos) {
        if (pseudo.pseudo_element) {
            ++result.types;
            continue;
        }
        if (pseudo.name == "where") {
            continue;
        }
        if ((pseudo.name == "is" || pseudo.name == "not" || pseudo.name == "has") &&
            pseudo.nested != nullptr) {
            result += specificity(*pseudo.nested);
            continue;
        }
        ++result.classes;
    }
    return result;
}

Specificity specificity(const ComplexSelector& selector) {
    Specificity result;
    for (const CompoundSelector& compound : selector.compounds) {
        result += specificity(compound);
    }
    return result;
}

Specificity specificity(const SelectorList& selectors) {
    Specificity result;
    for (const ComplexSelector& selector : selectors.selectors) {
        const Specificity current = specificity(selector);
        if (current > result) {
            result = current;
        }
    }
    return result;
}

} // namespace tessera::ui::style
