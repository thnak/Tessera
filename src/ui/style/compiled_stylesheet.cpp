// SPDX-License-Identifier: MIT

#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/selector.hpp"

#include <iterator>
#include <utility>

namespace tessera::ui::style {

CompiledStyleSheet::CompiledStyleSheet(StyleSheet sheet, Origin origin, PropertyRegistry registry)
    : origin_(origin),
      registry_(std::move(registry)) {
    rules_.reserve(sheet.rules.size());

    std::uint64_t source_order = 0;
    for (Rule& rule : sheet.rules) {
        if (rule.declarations.empty()) {
            continue;
        }

        const SelectorParseResult parsed = parse_selector_list(rule.selector);
        if (!parsed.ok || parsed.list.selectors.empty()) {
            continue;
        }

        PropertyExpansionResult expanded = registry_.expand(rule.declarations);
        diagnostics_.insert(diagnostics_.end(),
                            std::make_move_iterator(expanded.diagnostics.begin()),
                            std::make_move_iterator(expanded.diagnostics.end()));
        if (expanded.declarations.empty()) {
            continue;
        }

        const std::size_t rule_index = rules_.size();
        rules_.push_back(CompiledRule{std::move(expanded.declarations), source_order});
        source_order += rules_.back().declarations.size();
        index_.add(parsed.list, rule_index);
    }
}

void CompiledStyleSheet::add_to_cascade(const StyleNode& node, CascadeTable& cascade) const {
    for (const SelectorMatch& match : index_.match(node)) {
        if (match.value >= rules_.size()) {
            continue;
        }
        const CompiledRule& rule = rules_[match.value];
        std::uint64_t source_order = rule.source_order;
        cascade.add_declarations(rule.declarations, origin_, match.specificity, source_order);
    }
}

CascadeTable CompiledStyleSheet::cascade(const StyleNode& node) const {
    CascadeTable table;
    add_to_cascade(node, table);
    return table;
}

ComputedStyle CompiledStyleSheet::compute(const StyleNode& node,
                                          const ComputedStyle* parent) const {
    return registry_.compute_with_initial(parent, cascade(node));
}

CompiledStyleSheet compile_stylesheet(std::string_view source, Origin origin,
                                      PropertyRegistry registry) {
    return CompiledStyleSheet{parse_stylesheet(source), origin, std::move(registry)};
}

} // namespace tessera::ui::style
