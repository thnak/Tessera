// SPDX-License-Identifier: MIT

// JSX elements, fragments, attributes and children (docs/specs/jsx-subset-v1.md section 10).

#include "jsx_text.hpp"
#include "parser_internal.hpp"

namespace tessera::compiler::jsx {
namespace {

std::string suggest_intrinsic(const std::string& tag) {
    struct Alias {
        std::string_view html;
        std::string_view intrinsic;
    };
    static constexpr Alias kAliases[] = {
        {"div", "View"},          {"section", "View"}, {"main", "View"},
        {"span", "Text"},         {"p", "Text"},       {"label", "Text"},
        {"h1", "Text"},           {"h2", "Text"},      {"h3", "Text"},
        {"button", "Button"},     {"img", "Image"},    {"input", "TextInput"},
        {"textarea", "TextArea"}, {"path", "Path"},
    };
    for (const auto& alias : kAliases) {
        if (tag == alias.html) {
            return "use `<" + std::string(alias.intrinsic) + ">`";
        }
    }
    std::string capitalized = tag;
    // ASCII only: std::toupper would make the output depend on the process locale.
    if (capitalized[0] >= 'a' && capitalized[0] <= 'z') {
        capitalized[0] = static_cast<char>(capitalized[0] - 'a' + 'A');
    }
    return "tags start with an uppercase letter, for example `<" + capitalized + ">`";
}

} // namespace

ExprPtr Parser::parse_jsx_expression() {
    const std::uint32_t begin = cur_.span.offset;
    JsxPtr node = parse_jsx_element(LexMode::Code);
    if (!node) {
        return nullptr;
    }
    ExprPtr expr = make_expr(ExprKind::Jsx, begin);
    expr->jsx = std::move(node);
    return finish_node(*expr) ? std::move(expr) : nullptr;
}

// cur_ is the `<`. `after` is the mode of the token that follows the element: Code inside an
// expression, JsxChild when the element is a child of another element.
JsxPtr Parser::parse_jsx_element(LexMode after) {
    const std::uint32_t begin = cur_.span.offset;
    advance(LexMode::JsxTag);
    return parse_jsx_after_lt(begin, after);
}

JsxPtr Parser::parse_jsx_after_lt(std::uint32_t begin, LexMode after) {
    const DepthScope scope(*this);
    if (!scope.entered()) {
        return nullptr;
    }
    if (at_punct(">")) {
        return parse_fragment(begin, after);
    }
    if (cur_.kind == TokenKind::Identifier) {
        return parse_named_element(begin, after);
    }
    fail_unexpected("a tag name or `>`");
    return nullptr;
}

JsxPtr Parser::parse_fragment(std::uint32_t begin, LexMode after) {
    auto node = std::make_unique<JsxNode>();
    node->kind = JsxKind::Fragment;
    const Span opening{begin, cur_.span.end() - begin};
    advance(LexMode::JsxChild);
    if (!parse_children(*node, opening) || !parse_closing_tag(*node, opening, after)) {
        return nullptr;
    }
    node->span = span_from(begin);
    return finish_node(*node) ? std::move(node) : nullptr;
}

// Tag names are identifiers; `.` and `:` are consumed only to report the rejected member and
// namespace forms with the whole name.
bool Parser::parse_tag_name(std::string& name, Span& span) {
    const std::uint32_t begin = cur_.span.offset;
    name = std::string(cur_.text);
    advance(LexMode::JsxTag);
    while (at_punct(".") || at_punct(":")) {
        name += std::string(cur_.text);
        advance(LexMode::JsxTag);
        if (cur_.kind != TokenKind::Identifier) {
            fail_unexpected("a tag name");
            return false;
        }
        name += std::string(cur_.text);
        advance(LexMode::JsxTag);
    }
    span = span_from(begin);
    return true;
}

void Parser::check_tag_name(const std::string& name, Span span) {
    if (name.find_first_of(".:") != std::string::npos) {
        report("TESSERA_JSX_SUBSET_DYNAMIC_TAG",
               "member and namespace tags (`<" + excerpt(name) + ">`) are not supported", span,
               "use a fixed tag");
    } else if (name[0] >= 'a' && name[0] <= 'z') {
        report("TESSERA_JSX_SUBSET_HTML_TAG", "HTML tag `<" + excerpt(name) + ">` is not supported",
               span, suggest_intrinsic(excerpt(name)));
    }
}

JsxPtr Parser::parse_named_element(std::uint32_t begin, LexMode after) {
    auto node = std::make_unique<JsxNode>();
    node->kind = JsxKind::Element;
    if (!parse_tag_name(node->tag, node->tag_span)) {
        return nullptr;
    }
    check_tag_name(node->tag, node->tag_span);
    if (!parse_attributes(*node)) {
        return nullptr;
    }
    if (at_punct("/>")) {
        node->self_closing = true;
        advance(after);
    } else {
        advance(LexMode::JsxChild); // past `>`
        if (!parse_children(*node, node->tag_span) ||
            !parse_closing_tag(*node, node->tag_span, after)) {
            return nullptr;
        }
    }
    node->span = span_from(begin);
    return finish_node(*node) ? std::move(node) : nullptr;
}

// Reads children until the closing tag. On success cur_ is the token after the closing `<`
// (`/`, or `/>` for `</>`), lexed in tag mode, and closing_lt_ is the `<`.
bool Parser::parse_children(JsxNode& parent, Span opening) {
    while (!fatal_) {
        if (cur_.kind == TokenKind::Eof) {
            fail("TESSERA_JSX_PARSE_UNCLOSED_ELEMENT",
                 parent.kind == JsxKind::Fragment
                     ? "fragment `<>` is not closed: expected `</>`"
                     : "element `<" + excerpt(parent.tag) + ">` is not closed: expected `</" +
                           excerpt(parent.tag) + ">`",
                 opening);
            return false;
        }
        if (cur_.kind == TokenKind::JsxText) {
            add_text_child(parent);
        } else if (at_punct("{")) {
            add_container_child(parent);
        } else if (at_punct("<")) {
            const Span lt = cur_.span;
            advance(LexMode::JsxTag);
            if (at_punct("/") || at_punct("/>")) {
                closing_lt_ = lt;
                return true;
            }
            JsxPtr child = parse_jsx_after_lt(lt.offset, LexMode::JsxChild);
            if (!child) {
                return false;
            }
            parent.children.push_back(std::move(child));
        } else {
            fail_unexpected("a child, `{` or `<`");
        }
    }
    return false;
}

void Parser::add_text_child(JsxNode& parent) {
    const std::string_view raw = cur_.text;
    std::vector<EntityError> errors;
    decode_entities(raw, &errors);
    for (const auto& error : errors) {
        report("TESSERA_JSX_PARSE_UNKNOWN_ENTITY", error.message,
               Span{static_cast<std::uint32_t>(cur_.span.offset + error.offset),
                    static_cast<std::uint32_t>(error.length)});
    }
    const std::string collapsed = collapse_jsx_text(raw);
    if (!collapsed.empty()) {
        auto node = std::make_unique<JsxNode>();
        node->kind = JsxKind::Text;
        node->span = cur_.span;
        node->text = decode_entities(collapsed, nullptr);
        parent.children.push_back(std::move(node));
    }
    advance(LexMode::JsxChild);
}

void Parser::add_container_child(JsxNode& parent) {
    const std::uint32_t begin = cur_.span.offset;
    advance(LexMode::Code);
    auto node = std::make_unique<JsxNode>();
    node->kind = JsxKind::ExprContainer;
    if (!at_punct("}")) { // `{}` and `{/* comment */}` are empty containers
        node->expr = parse_expr();
        if (!node->expr) {
            return;
        }
        if (!at_punct("}")) {
            fail_unexpected("`}`");
            return;
        }
    }
    advance(LexMode::JsxChild);
    node->span = span_from(begin);
    if (finish_node(*node)) {
        parent.children.push_back(std::move(node));
    }
}

bool Parser::parse_closing_tag(const JsxNode& element, Span opening, LexMode after) {
    const bool is_fragment = element.kind == JsxKind::Fragment;
    const std::string open_name = is_fragment ? "<>" : "<" + excerpt(element.tag) + ">";
    const auto mismatch = [&](const std::string& close_name, Span where) {
        sink_.report("TESSERA_JSX_PARSE_MISMATCHED_TAG", Severity::Error,
                     "closing tag `" + excerpt(close_name) + "` does not match `" + open_name + "`",
                     where,
                     "close it with " +
                         (is_fragment ? std::string("`</>`") : "`</" + excerpt(element.tag) + ">`"),
                     RelatedSpan{map_.resolve(opening), "`" + open_name + "` is opened here"});
    };
    if (at_punct("/>")) { // `</>` lexes as `<` then `/>`
        if (!is_fragment) {
            mismatch("</>", Span{closing_lt_.offset, cur_.span.end() - closing_lt_.offset});
        }
        advance(after);
        return true;
    }
    advance(LexMode::JsxTag); // past `/`
    std::string name;
    Span name_span = cur_.span;
    if (cur_.kind == TokenKind::Identifier && !parse_tag_name(name, name_span)) {
        return false;
    }
    if (!at_punct(">")) {
        fail_unexpected("`>` to end the closing tag");
        return false;
    }
    if (is_fragment ? !name.empty() : name != element.tag) {
        mismatch("</" + name + ">", Span{closing_lt_.offset, cur_.span.end() - closing_lt_.offset});
    }
    advance(after);
    return true;
}

} // namespace tessera::compiler::jsx
