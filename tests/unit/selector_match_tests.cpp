// SPDX-License-Identifier: MIT

#include "tessera/ui/style/selector_match.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using tessera::ui::style::matches;
using tessera::ui::style::parse_selector_list;
using tessera::ui::style::PseudoState;
using tessera::ui::style::SelectorIndex;
using tessera::ui::style::SelectorMatch;
using tessera::ui::style::StyleNode;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "selector_match: " << message << "\n";
    }
    return condition;
}

void attach(StyleNode& parent, StyleNode& child) {
    child.parent = &parent;
    parent.children.push_back(&child);
}

bool matches_text(std::string_view text, const StyleNode& node) {
    const auto parsed = parse_selector_list(text);
    return parsed.ok && matches(parsed.list, node);
}

bool has_value(const std::vector<SelectorMatch>& matches, std::size_t value) {
    for (const SelectorMatch& match : matches) {
        if (match.value == value) {
            return true;
        }
    }
    return false;
}

const SelectorMatch* find_match(const std::vector<SelectorMatch>& matches, std::size_t value) {
    for (const SelectorMatch& match : matches) {
        if (match.value == value) {
            return &match;
        }
    }
    return nullptr;
}

} // namespace

int main() {
    StyleNode root;
    root.tag = "root";
    root.id = "app";

    StyleNode button;
    button.tag = "button";
    button.id = "save";
    button.classes = {"primary", "big"};
    button.attributes = {{"type", "button"}, {"class", "primary big"}};
    button.state = PseudoState::Hover | PseudoState::Focus | PseudoState::FocusVisible;

    StyleNode card;
    card.tag = "div";
    card.id = "main";
    card.classes = {"card"};
    card.attributes = {{"data-role", "tab"},
                       {"lang", "en-US"},
                       {"href", "https://example.com/file.pdf"},
                       {"title", "hello world"}};

    StyleNode title;
    title.tag = "span";
    title.classes = {"title"};

    StyleNode input;
    input.tag = "input";
    input.classes = {"field"};
    input.attributes = {{"disabled", ""}};
    input.state = PseudoState::Disabled | PseudoState::Required;

    StyleNode empty;
    empty.tag = "i";

    StyleNode text;
    text.tag = "p";
    text.has_text = true;

    attach(root, button);
    attach(root, card);
    attach(root, input);
    attach(root, empty);
    attach(root, text);
    attach(card, title);

    if (!expect(matches_text(".card", card), "class match") ||
        !expect(matches_text("button#save.primary", button), "type id class") ||
        !expect(!matches_text(".card", button), "class mismatch") ||
        !expect(matches_text("*", input), "universal")) {
        return 1;
    }

    if (!expect(matches_text(".card .title", title), "descendant") ||
        !expect(matches_text(".card > .title", title), "child") ||
        !expect(!matches_text(".card > button", button), "child mismatch") ||
        !expect(matches_text(".card + .field", input), "adjacent sibling") ||
        !expect(matches_text(".card ~ .field", input), "general sibling") ||
        !expect(!matches_text(".field ~ .card", card), "reverse general sibling")) {
        return 2;
    }

    if (!expect(matches_text("[disabled]", input), "attribute exists") ||
        !expect(!matches_text("[disabled]", button), "attribute missing") ||
        !expect(matches_text("[data-role=\"tab\"]", card), "attribute equals") ||
        !expect(matches_text("[lang|=en]", card), "attribute dash") ||
        !expect(matches_text("[class~=\"big\"]", button), "attribute includes") ||
        !expect(matches_text("[href^=\"https\"]", card), "attribute prefix") ||
        !expect(matches_text("[href$=\".pdf\"]", card), "attribute suffix") ||
        !expect(matches_text("[title*=\"hello\"]", card), "attribute substring") ||
        !expect(matches_text("[type=\"BUTTON\" i]", button), "attribute insensitive")) {
        return 3;
    }

    if (!expect(matches_text("button:hover", button), "hover") ||
        !expect(!matches_text("button:active", button), "active false") ||
        !expect(matches_text("button:focus", button), "focus") ||
        !expect(matches_text("button:focus-visible", button), "focus-visible") ||
        !expect(matches_text("input:disabled", input), "disabled") ||
        !expect(!matches_text("input:enabled", input), "enabled false") ||
        !expect(matches_text("button:enabled", button), "enabled") ||
        !expect(matches_text("input:required", input), "required") ||
        !expect(matches_text("button:optional", button), "optional") ||
        !expect(!matches_text("button:visited", button), "visited false")) {
        return 4;
    }

    if (!expect(matches_text(":root", root), "root") ||
        !expect(!matches_text("div:root", card), "non-root") ||
        !expect(matches_text("i:empty", empty), "empty") ||
        !expect(!matches_text("p:empty", text), "text is not empty") ||
        !expect(!matches_text("div:empty", card), "children are not empty") ||
        !expect(matches_text("button:first-child", button), "first-child") ||
        !expect(!matches_text("div:first-child", card), "first-child false") ||
        !expect(matches_text("p:last-child", text), "last-child") ||
        !expect(matches_text("span:only-child", title), "only-child") ||
        !expect(!matches_text("div:only-child", card), "only-child false")) {
        return 5;
    }

    if (!expect(matches_text("div:first-of-type", card), "first-of-type") ||
        !expect(matches_text("div:last-of-type", card), "last-of-type") ||
        !expect(matches_text("span:first-of-type", title), "span first-of-type") ||
        !expect(!matches_text("span:last-of-type", button), "last-of-type false") ||
        !expect(matches_text("div:nth-child(2)", card), "nth-child") ||
        !expect(matches_text("div:nth-child(2n)", card), "nth-child formula") ||
        !expect(!matches_text("div:nth-child(3)", card), "nth-child mismatch") ||
        !expect(matches_text("div:nth-last-child(4)", card), "nth-last-child") ||
        !expect(matches_text("div:nth-child(-n+3)", card), "nth negative") ||
        !expect(matches_text("input:nth-child(odd)", input), "nth odd")) {
        return 6;
    }

    if (!expect(matches_text(":not(.card)", button), "not") ||
        !expect(!matches_text(":not(.card)", card), "not false") ||
        !expect(matches_text(":is(.card, #save)", card), "is") ||
        !expect(matches_text(":where(.card)", card), "where") ||
        !expect(!matches_text("div:has(.title)", card), "has deferred")) {
        return 7;
    }

    SelectorIndex index;
    index.add(parse_selector_list(".card").list, 10);
    index.add(parse_selector_list("#save").list, 20);
    index.add(parse_selector_list("button").list, 30);
    index.add(parse_selector_list(".card .title").list, 40);
    index.add(parse_selector_list("[disabled]").list, 50);
    index.add(parse_selector_list("*").list, 60);
    if (!expect(index.size() == 6, "index size")) {
        return 8;
    }

    auto found = index.match(button);
    if (!expect(has_value(found, 20) && has_value(found, 30) && has_value(found, 60),
                "button index candidates") ||
        !expect(!has_value(found, 10), "button class filter")) {
        return 9;
    }

    found = index.match(card);
    if (!expect(has_value(found, 10) && has_value(found, 60), "card index candidates") ||
        !expect(!has_value(found, 20), "card id filter")) {
        return 10;
    }

    found = index.match(title);
    if (!expect(has_value(found, 40) && has_value(found, 60), "title index candidates")) {
        return 11;
    }
    const SelectorMatch* title_match = find_match(found, 40);
    if (!expect(title_match != nullptr && title_match->specificity.classes == 2,
                "indexed specificity")) {
        return 12;
    }

    found = index.match(input);
    if (!expect(has_value(found, 50) && has_value(found, 60), "input index candidates") ||
        !expect(!has_value(found, 30), "input tag filter")) {
        return 13;
    }

    SelectorIndex list_index;
    list_index.add(parse_selector_list(".card, #save").list, 70);
    found = list_index.match(button);
    const SelectorMatch* list_match = find_match(found, 70);
    if (!expect(list_match != nullptr && list_match->specificity.ids == 1,
                "list index button specificity")) {
        return 14;
    }
    found = list_index.match(card);
    list_match = find_match(found, 70);
    if (!expect(list_match != nullptr && list_match->specificity.classes == 1,
                "list index card specificity")) {
        return 15;
    }

    list_index.clear();
    if (!expect(list_index.size() == 0 && list_index.match(card).empty(), "index clear")) {
        return 16;
    }

    std::cout << "selector_match: ok\n";
    return 0;
}
