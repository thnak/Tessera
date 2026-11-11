// SPDX-License-Identifier: MIT

#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/selector.hpp"
#include "tessera/ui/style/supports.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

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

} // namespace

CompiledStyleSheet::CompiledStyleSheet(StyleSheet sheet, Origin origin, PropertyRegistry registry,
                                       MediaContext media)
    : origin_(origin),
      registry_(std::move(registry)),
      media_(media) {
    std::uint64_t source_order = 0;
    compile_rules(sheet, source_order, kUnlayeredLayer);
}

void CompiledStyleSheet::compile_rules(const StyleSheet& sheet, std::uint64_t& source_order,
                                       std::uint32_t layer) {
    struct Item {
        std::size_t offset = 0;
        const Rule* rule = nullptr;
        const AtRule* at_rule = nullptr;
    };

    std::vector<Item> items;
    items.reserve(sheet.rules.size() + sheet.at_rules.size());
    for (const Rule& rule : sheet.rules) {
        items.push_back(Item{rule.offset, &rule, nullptr});
    }
    for (const AtRule& at_rule : sheet.at_rules) {
        items.push_back(Item{at_rule.offset, nullptr, &at_rule});
    }
    std::stable_sort(items.begin(), items.end(),
                     [](const Item& lhs, const Item& rhs) { return lhs.offset < rhs.offset; });

    for (const Item& item : items) {
        if (item.rule != nullptr) {
            compile_rule(*item.rule, source_order, layer);
        } else if (item.at_rule != nullptr) {
            compile_at_rule(*item.at_rule, source_order, layer);
        }
    }
}

void CompiledStyleSheet::compile_rule(const Rule& rule, std::uint64_t& source_order,
                                      std::uint32_t layer) {
    if (rule.declarations.empty()) {
        return;
    }

    const SelectorParseResult parsed = parse_selector_list(rule.selector);
    if (!parsed.ok || parsed.list.selectors.empty()) {
        return;
    }

    PropertyExpansionResult expanded = registry_.expand(rule.declarations);
    diagnostics_.insert(diagnostics_.end(), std::make_move_iterator(expanded.diagnostics.begin()),
                        std::make_move_iterator(expanded.diagnostics.end()));
    if (expanded.declarations.empty()) {
        return;
    }

    const std::size_t rule_index = rules_.size();
    rules_.push_back(
        CompiledRule{std::move(expanded.declarations), source_order, layer, active_containers_});
    has_container_queries_ = has_container_queries_ || !active_containers_.empty();
    source_order += rules_.back().declarations.size();
    index_.add(parsed.list, rule_index);
}

void CompiledStyleSheet::compile_at_rule(const AtRule& at_rule, std::uint64_t& source_order,
                                         std::uint32_t layer) {
    const std::string name = lower(at_rule.name);
    if (name == "media") {
        const MediaQueryParseResult query = parse_media_query_list(at_rule.prelude);
        if (!query.ok) {
            diagnostics_.push_back(StyleDiagnostic{StyleDiagnosticKind::InvalidAtRule,
                                                   at_rule.offset, name, "invalid media query"});
            return;
        }
        if (evaluate(query.list, media_)) {
            compile_rules(parse_stylesheet(at_rule.block), source_order, layer);
        }
        return;
    }

    if (name == "container") {
        ContainerQueryParseResult query = parse_container_query(at_rule.prelude);
        if (!query.ok) {
            diagnostics_.push_back(StyleDiagnostic{StyleDiagnosticKind::InvalidAtRule,
                                                   at_rule.offset, name,
                                                   "invalid container query"});
            return;
        }
        active_containers_.push_back(std::move(query.query));
        compile_rules(parse_stylesheet(at_rule.block), source_order, layer);
        active_containers_.pop_back();
        return;
    }

    if (name == "layer") {
        std::vector<std::string> names;
        std::size_t start = 0;
        while (start <= at_rule.prelude.size()) {
            const std::size_t comma = at_rule.prelude.find(',', start);
            const std::size_t end = comma == std::string::npos ? at_rule.prelude.size() : comma;
            const std::string part = trim(at_rule.prelude.substr(start, end - start));
            if (!part.empty()) {
                names.push_back(part);
            }
            if (comma == std::string::npos) {
                break;
            }
            start = comma + 1;
        }

        std::uint32_t target = layer;
        if (!at_rule.has_block) {
            for (const std::string& layer_name : names) {
                (void)layer_for(layer_name);
            }
            return;
        }
        if (names.empty()) {
            target = layer_for(std::string_view{});
        } else {
            bool assigned = false;
            for (const std::string& layer_name : names) {
                const std::uint32_t id = layer_for(layer_name);
                if (!assigned) {
                    target = id;
                    assigned = true;
                }
            }
        }
        compile_rules(parse_stylesheet(at_rule.block), source_order, target);
        return;
    }

    if (name == "supports") {
        const SupportsParseResult condition = parse_supports_condition(at_rule.prelude);
        if (!condition.ok) {
            diagnostics_.push_back(StyleDiagnostic{StyleDiagnosticKind::InvalidAtRule,
                                                   at_rule.offset, name,
                                                   "invalid supports condition"});
            return;
        }
        if (evaluate_supports(condition.condition, registry_)) {
            compile_rules(parse_stylesheet(at_rule.block), source_order, layer);
        }
        return;
    }

    if (name == "keyframes") {
        KeyframesParseResult keyframes = parse_keyframes(at_rule);
        if (!keyframes.ok) {
            diagnostics_.push_back(StyleDiagnostic{StyleDiagnosticKind::InvalidAtRule,
                                                   at_rule.offset, name, "invalid keyframes"});
        }
        if (!keyframes.keyframes.steps.empty()) {
            keyframes_.push_back(std::move(keyframes.keyframes));
        }
    }
}

void CompiledStyleSheet::add_to_cascade(const StyleNode& node, CascadeTable& cascade,
                                        const ContainerSizeSource* containers) const {
    for (const SelectorMatch& match : index_.match(node)) {
        if (match.value >= rules_.size()) {
            continue;
        }
        const CompiledRule& rule = rules_[match.value];
        if (!rule.containers.empty()) {
            const bool applies = containers != nullptr &&
                                 std::all_of(rule.containers.begin(), rule.containers.end(),
                                             [&](const ContainerQuery& query) {
                                                 ContainerSize size;
                                                 return containers->find(node, query.name, size) &&
                                                        evaluate(query, size);
                                             });
            if (!applies) {
                continue;
            }
        }
        std::uint64_t source_order = rule.source_order;
        cascade.add_declarations(rule.declarations, origin_, match.specificity, source_order,
                                 rule.layer);
    }
}

std::uint32_t CompiledStyleSheet::layer_for(std::string_view name) {
    if (name.empty()) {
        return next_layer_++;
    }
    const std::string key(name);
    const auto found = layer_order_.find(key);
    if (found != layer_order_.end()) {
        return found->second;
    }
    const std::uint32_t order = next_layer_++;
    layer_order_.emplace(key, order);
    return order;
}

CascadeTable CompiledStyleSheet::cascade(const StyleNode& node,
                                         const ContainerSizeSource* containers) const {
    CascadeTable table;
    add_to_cascade(node, table, containers);
    return table;
}

ComputedStyle CompiledStyleSheet::compute(const StyleNode& node, const ComputedStyle* parent,
                                          const ContainerSizeSource* containers) const {
    return registry_.compute_with_initial(parent, cascade(node, containers));
}

CompiledStyleSheet compile_stylesheet(std::string_view source, Origin origin,
                                      PropertyRegistry registry, MediaContext media) {
    return CompiledStyleSheet{parse_stylesheet(source), origin, std::move(registry), media};
}

} // namespace tessera::ui::style
