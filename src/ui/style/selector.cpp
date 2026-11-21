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

// The nesting budget for functional pseudo-classes, matching blob_io.hpp's kMaxSelectorDepth: a
// selector blob and a selector string must not disagree about how deep a selector may be. Exceeding
// it is a parse failure, not a crash -- `:not(:not(:not(...)))` is not something a real stylesheet
// writes, and a hostile one must not be able to abort the process.
inline constexpr int kMaxNestedSelectorDepth = 16;

// Declared here because a nested functional pseudo-class calls back into it before the
// definition further down this file.
SelectorParseResult parse_selector_list_at(std::string_view text, int depth);

class SelectorParser {
public:
    explicit SelectorParser(std::vector<Token> tokens, int depth = 0)
        : tokens_(std::move(tokens)),
          depth_(depth) {}

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
        // A relative selector (the argument of `:has()`, issue #622) may start with a combinator:
        // `:has(> .icon)` is a child and `:has(+ .a)` an adjacent sibling. The leading combinator
        // is recorded against an implicit universal compound, so the matcher sees the same shape a
        // written-out `* > .icon` would have.
        if (!at_end() &&
            (is_delim(peek(), '>') || is_delim(peek(), '+') || is_delim(peek(), '~'))) {
            const char value = peek().text.front();
            const Combinator combinator = value == '>'   ? Combinator::Child
                                          : value == '+' ? Combinator::AdjacentSibling
                                                         : Combinator::GeneralSibling;
            take();
            skip_trivia(nullptr);
            complex.compounds.push_back(CompoundSelector{});
            complex.combinators.push_back(combinator);
        }

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
                if (depth_ >= kMaxNestedSelectorDepth) {
                    // Too deep to be a real selector; refuse rather than overflow the stack.
                    return false;
                }
                SelectorParseResult nested = parse_selector_list_at(pseudo.argument, depth_ + 1);
                if (!nested.ok) {
                    // A functional pseudo-class whose argument does not parse cannot be honoured,
                    // and dropping just the argument would silently change what the selector means.
                    // Fail the compound instead.
                    return false;
                }
                pseudo.nested = std::make_shared<const SelectorList>(std::move(nested.list));
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
    // How many functional pseudo-classes enclose this parse; see kMaxNestedSelectorDepth.
    int depth_ = 0;
};

} // namespace

namespace {

SelectorParseResult parse_selector_list_at(std::string_view text, int depth) {
    Tokenizer tokenizer(text);
    SelectorParser parser(tokenizer.tokenize(), depth);
    return parser.parse();
}

} // namespace

SelectorParseResult parse_selector_list(std::string_view text) {
    return parse_selector_list_at(text, 0);
}

namespace {

bool structural_pseudo(std::string_view name) {
    for (const std::string_view structural :
         {"has", "nth-child", "nth-last-child", "nth-of-type", "nth-last-of-type", "first-child",
          "last-child", "only-child", "first-of-type", "last-of-type", "only-of-type", "empty"}) {
        if (name == structural) {
            return true;
        }
    }
    return false;
}

} // namespace

bool selector_is_subtree_local(const ComplexSelector& complex) {
    for (const Combinator combinator : complex.combinators) {
        if (combinator == Combinator::AdjacentSibling || combinator == Combinator::GeneralSibling) {
            return false;
        }
    }
    for (const CompoundSelector& compound : complex.compounds) {
        for (const PseudoClass& pseudo : compound.pseudos) {
            if (structural_pseudo(pseudo.name) ||
                (pseudo.nested != nullptr && !selector_list_is_subtree_local(*pseudo.nested))) {
                return false;
            }
        }
    }
    return true;
}

bool selector_list_is_subtree_local(const SelectorList& list) {
    for (const ComplexSelector& complex : list.selectors) {
        if (!selector_is_subtree_local(complex)) {
            return false;
        }
    }
    return true;
}

} // namespace tessera::ui::style
