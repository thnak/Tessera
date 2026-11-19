// SPDX-License-Identifier: MIT

#include "tessera/ui/style/prepared_stylesheet.hpp"
#include "tessera/ui/style/supports.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <utility>

namespace tessera::ui::style {
namespace {

std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::string trim(std::string_view value) {
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

class Preparer {
public:
    Preparer(const PropertyRegistry& registry, PreparedStyleSheet& out)
        : registry_(registry),
          out_(out) {}

    // The nesting budget for at-rules, matching the selector parser's bound in spirit: a stylesheet
    // must not be able to abort the compiler by nesting `@media` inside `@media`. Exceeding it is a
    // diagnostic, not a crash.
    static constexpr int kMaxAtRuleDepth = 32;

    void rules(const StyleSheet& sheet, std::uint32_t layer, int depth = 0);

private:
    struct Item {
        std::size_t offset = 0;
        const Rule* rule = nullptr;
        const AtRule* at_rule = nullptr;
    };

    void rule(const Rule& source, std::uint32_t layer);
    void at_rule(const AtRule& source, std::uint32_t layer, int depth = 0);
    void layer_at_rule(const AtRule& source, std::uint32_t layer, int depth = 0);
    std::uint32_t layer_for(std::string_view name);
    void diagnose(const AtRule& source, std::string_view name, const char* message);
    PreparedItem make_item(PreparedItemKind kind) const;

    const PropertyRegistry& registry_;
    PreparedStyleSheet& out_;
    std::vector<MediaQueryList> media_;
    std::vector<ContainerQuery> containers_;
};

PreparedItem Preparer::make_item(PreparedItemKind kind) const {
    PreparedItem item;
    item.kind = kind;
    item.media = media_;
    return item;
}

void Preparer::diagnose(const AtRule& source, std::string_view name, const char* message) {
    out_.diagnostics.push_back(StyleDiagnostic{StyleDiagnosticKind::InvalidAtRule, source.offset,
                                               std::string(name), message});
}

std::uint32_t Preparer::layer_for(std::string_view name) {
    if (!name.empty()) {
        const auto found = std::find(out_.layers.begin(), out_.layers.end(), name);
        if (found != out_.layers.end()) {
            return static_cast<std::uint32_t>(found - out_.layers.begin());
        }
    }
    out_.layers.emplace_back(name);
    return static_cast<std::uint32_t>(out_.layers.size() - 1);
}

void Preparer::rules(const StyleSheet& sheet, std::uint32_t layer, int depth) {
    std::vector<Item> items;
    items.reserve(sheet.rules.size() + sheet.at_rules.size());
    for (const Rule& entry : sheet.rules) {
        items.push_back(Item{entry.offset, &entry, nullptr});
    }
    for (const AtRule& entry : sheet.at_rules) {
        items.push_back(Item{entry.offset, nullptr, &entry});
    }
    std::stable_sort(items.begin(), items.end(),
                     [](const Item& lhs, const Item& rhs) { return lhs.offset < rhs.offset; });

    for (const Item& item : items) {
        if (item.rule != nullptr) {
            rule(*item.rule, layer);
        } else if (item.at_rule != nullptr) {
            at_rule(*item.at_rule, layer, depth);
        }
    }
}

void Preparer::rule(const Rule& source, std::uint32_t layer) {
    if (source.declarations.empty()) {
        return;
    }
    SelectorParseResult parsed = parse_selector_list(source.selector);
    if (!parsed.ok || parsed.list.selectors.empty()) {
        return;
    }
    PropertyExpansionResult expanded = registry_.expand(source.declarations);
    out_.diagnostics.insert(out_.diagnostics.end(),
                            std::make_move_iterator(expanded.diagnostics.begin()),
                            std::make_move_iterator(expanded.diagnostics.end()));
    if (expanded.declarations.empty()) {
        return;
    }
    PreparedItem item = make_item(PreparedItemKind::Rule);
    item.selectors = std::move(parsed.list);
    // The authored text and the rule's offset, kept for the designer's `style.info`
    // (designer-protocol-v1 section 9.1). `source.selector` is already trimmed by the parser, so it
    // is exactly what a developer sees before the `{`.
    item.selector = source.selector;
    item.offset = source.offset;
    item.declarations = std::move(expanded.declarations);
    item.layer = layer;
    item.containers = containers_;
    out_.items.push_back(std::move(item));
}

void Preparer::layer_at_rule(const AtRule& source, std::uint32_t layer, int depth) {
    if (depth >= kMaxAtRuleDepth) {
        diagnose(source, "layer", "at-rules nested too deeply");
        return;
    }
    std::vector<std::string> names;
    std::size_t start = 0;
    while (start <= source.prelude.size()) {
        const std::size_t comma = source.prelude.find(',', start);
        const std::size_t end = comma == std::string::npos ? source.prelude.size() : comma;
        std::string part = trim(source.prelude.substr(start, end - start));
        if (!part.empty()) {
            names.push_back(std::move(part));
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }

    PreparedItem item = make_item(PreparedItemKind::Layer);
    std::uint32_t target = layer;
    if (!source.has_block) {
        for (const std::string& name : names) {
            item.layer_refs.push_back(layer_for(name));
        }
        out_.items.push_back(std::move(item));
        return;
    }
    if (names.empty()) {
        target = layer_for(std::string_view{});
        item.layer_refs.push_back(target);
    } else {
        for (std::size_t i = 0; i < names.size(); ++i) {
            const std::uint32_t id = layer_for(names[i]);
            item.layer_refs.push_back(id);
            if (i == 0) {
                target = id;
            }
        }
    }
    out_.items.push_back(std::move(item));
    rules(parse_stylesheet(source.block), target, depth + 1);
}

void Preparer::at_rule(const AtRule& source, std::uint32_t layer, int depth) {
    if (depth >= kMaxAtRuleDepth) {
        // Refuse rather than recurse: a stylesheet must not be able to overflow the stack by
        // nesting at-rules. The rule is dropped with a diagnostic, like any other bad rule.
        diagnose(source, lower(source.name), "at-rules nested too deeply");
        return;
    }
    const std::string name = lower(source.name);
    if (name == "media") {
        MediaQueryParseResult query = parse_media_query_list(source.prelude);
        if (!query.ok) {
            diagnose(source, name, "invalid media query");
            return;
        }
        media_.push_back(std::move(query.list));
        rules(parse_stylesheet(source.block), layer, depth + 1);
        media_.pop_back();
        return;
    }
    if (name == "container") {
        ContainerQueryParseResult query = parse_container_query(source.prelude);
        if (!query.ok) {
            diagnose(source, name, "invalid container query");
            return;
        }
        PreparedItem item = make_item(PreparedItemKind::Container);
        item.container = query.query;
        out_.items.push_back(std::move(item));
        containers_.push_back(std::move(query.query));
        rules(parse_stylesheet(source.block), layer, depth + 1);
        containers_.pop_back();
        return;
    }
    if (name == "layer") {
        layer_at_rule(source, layer, depth);
        return;
    }
    if (name == "supports") {
        const SupportsParseResult condition = parse_supports_condition(source.prelude);
        if (!condition.ok) {
            // The whole block is dropped, which is the "a failed nested parse fails the compound"
            // rule: a condition the parser could not finish must not apply half its rules. The
            // message names both causes because a condition that exceeds the nesting budget is
            // refused by the same path as a malformed one (see kMaxSupportsDepth).
            diagnose(source, name, "invalid or too deeply nested supports condition");
            return;
        }
        if (evaluate_supports(condition.condition, registry_)) {
            rules(parse_stylesheet(source.block), layer, depth + 1);
        }
        return;
    }
    if (name == "keyframes") {
        KeyframesParseResult keyframes = parse_keyframes(source);
        if (!keyframes.ok) {
            diagnose(source, name, "invalid keyframes");
        }
        if (!keyframes.keyframes.steps.empty()) {
            PreparedItem item = make_item(PreparedItemKind::Keyframes);
            item.keyframes = std::move(keyframes.keyframes);
            out_.items.push_back(std::move(item));
        }
    }
}

} // namespace

std::uint64_t fnv1a64(std::string_view bytes) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char c : bytes) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

PreparedStyleSheet prepare_stylesheet(const StyleSheet& sheet, const PropertyRegistry& registry) {
    PreparedStyleSheet prepared;
    Preparer(registry, prepared).rules(sheet, kUnlayeredLayer);
    return prepared;
}

PreparedStyleSheet prepare_stylesheet(std::string_view source, const PropertyRegistry& registry) {
    PreparedStyleSheet prepared = prepare_stylesheet(parse_stylesheet(source), registry);
    prepared.source_hash = fnv1a64(source);
    return prepared;
}

bool PreparedStyleSheet::media_answer_can_change(const MediaContext& before,
                                                 const MediaContext& after) const {
    // The same `evaluate(list, context)` call the resolve step (`CompiledStyleSheet::resolve`)
    // makes, so this cannot disagree with the cascade about which rules apply: an item's
    // participation is determined by that boolean alone. Two contexts that agree on every list
    // produce the same cascade, so a resize between them is a layout pass and nothing more.
    for (const PreparedItem& item : items) {
        for (const MediaQueryList& list : item.media) {
            if (evaluate(list, before) != evaluate(list, after)) {
                return true;
            }
        }
    }
    return false;
}

} // namespace tessera::ui::style
