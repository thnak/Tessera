// SPDX-License-Identifier: MIT

#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace tessera::ui::style {

CompiledStyleSheet::CompiledStyleSheet(StyleSheet sheet, Origin origin, PropertyRegistry registry,
                                       MediaContext media)
    : origin_(origin),
      registry_(std::move(registry)),
      media_(media) {
    resolve(prepare_stylesheet(sheet, registry_));
}

CompiledStyleSheet::CompiledStyleSheet(const PreparedStyleSheet& prepared, Origin origin,
                                       PropertyRegistry registry, MediaContext media)
    : origin_(origin),
      registry_(std::move(registry)),
      media_(media) {
    resolve(prepared);
}

void CompiledStyleSheet::resolve(const PreparedStyleSheet& prepared) {
    constexpr std::uint32_t kUnassigned = UINT32_MAX;
    std::vector<std::uint32_t> layer_ids(prepared.layers.size(), kUnassigned);
    std::uint32_t next_layer = 0;
    // Layer order follows the first applicable appearance, so a layer that only appears inside a
    // non-matching `@media` never takes a slot.
    const auto assign_layer = [&](std::uint32_t index) {
        if (index >= layer_ids.size()) {
            return kUnlayeredLayer;
        }
        if (layer_ids[index] == kUnassigned) {
            layer_ids[index] = next_layer++;
            if (!prepared.layers[index].empty()) {
                layer_order_.emplace(prepared.layers[index], layer_ids[index]);
            }
        }
        return layer_ids[index];
    };

    diagnostics_ = prepared.diagnostics;
    std::uint64_t source_order = 0;
    for (const PreparedItem& item : prepared.items) {
        const bool applies =
            std::all_of(item.media.begin(), item.media.end(),
                        [&](const MediaQueryList& list) { return evaluate(list, media_); });
        if (!applies) {
            continue;
        }
        switch (item.kind) {
        case PreparedItemKind::Rule: {
            const std::size_t rule_index = rules_.size();
            const std::uint32_t layer =
                item.layer == kUnlayeredLayer ? kUnlayeredLayer : assign_layer(item.layer);
            rules_.push_back(CompiledRule{item.declarations, source_order, layer, item.containers});
            has_container_queries_ = has_container_queries_ || !item.containers.empty();
            source_order += item.declarations.size();
            index_.add(item.selectors, rule_index);
            break;
        }
        case PreparedItemKind::Layer:
            for (const std::uint32_t ref : item.layer_refs) {
                (void)assign_layer(ref);
            }
            break;
        case PreparedItemKind::Container:
            container_queries_.push_back(item.container);
            break;
        case PreparedItemKind::Keyframes:
            keyframes_.push_back(item.keyframes);
            break;
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
