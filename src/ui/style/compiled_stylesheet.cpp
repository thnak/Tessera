// SPDX-License-Identifier: MIT

#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

namespace tessera::ui::style {
namespace {

bool is_tessera_ui_layer(std::string_view name) {
    return name == "tessera-ui" || name.starts_with("tessera-ui.");
}

bool has_keyframes_named(const std::vector<KeyframeBlock>& blocks, std::string_view name) {
    return std::any_of(blocks.begin(), blocks.end(),
                       [name](const KeyframeBlock& block) { return block.name == name; });
}

} // namespace

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

CompiledStyleSheet::CompiledStyleSheet(std::span<const StylesheetSource> sheets,
                                       PropertyRegistry registry, MediaContext media)
    : registry_(std::move(registry)),
      media_(media) {
    if (!sheets.empty()) {
        origin_ = sheets.back().origin;
    }
    // Library CSS first: `@layer tessera-ui` and its nested layers must take the lowest slots, so
    // every app layer (layered or not) outranks them. Inside each pass the given order is kept,
    // which is the import-graph order of the app sheets.
    for (const bool library : std::array<bool, 2>{true, false}) {
        for (const StylesheetSource& source : sheets) {
            if (source.prepared == nullptr || source.library != library) {
                continue;
            }
            append_sheet(*source.prepared, source.origin, source.library);
        }
    }
}

std::uint32_t CompiledStyleSheet::layer_slot(std::string_view name, std::string_view prefix) {
    std::string key;
    if (!prefix.empty()) {
        key.reserve(prefix.size() + 1 + name.size());
        key.append(prefix);
        key.push_back('.');
    }
    key.append(name);
    const auto found = layer_order_.find(key);
    if (found != layer_order_.end()) {
        return found->second;
    }
    const std::uint32_t slot = next_layer_++;
    layer_order_.emplace(std::move(key), slot);
    return slot;
}

void CompiledStyleSheet::resolve(const PreparedStyleSheet& prepared) {
    constexpr std::uint32_t kUnassigned = UINT32_MAX;
    std::vector<std::uint32_t> layer_ids(prepared.layers.size(), kUnassigned);
    // Layer order follows the first applicable appearance, so a layer that only appears inside a
    // non-matching `@media` never takes a slot.
    const auto assign_layer = [&](std::uint32_t index) {
        if (index >= layer_ids.size()) {
            return kUnlayeredLayer;
        }
        if (layer_ids[index] == kUnassigned) {
            const std::string& name = prepared.layers[index];
            layer_ids[index] = name.empty() ? next_layer_++ : layer_slot(name, {});
        }
        return layer_ids[index];
    };

    diagnostics_ = prepared.diagnostics;
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
            rules_.push_back(CompiledRule{item.declarations, next_source_order_, layer,
                                          item.containers, origin_});
            has_container_queries_ = has_container_queries_ || !item.containers.empty();
            next_source_order_ += item.declarations.size();
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

void CompiledStyleSheet::append_sheet(const PreparedStyleSheet& prepared, Origin origin,
                                      bool library) {
    // Resolve into a temporary and merge the resolved form, so the cascade (and the layer remap) is
    // implemented once.
    const CompiledStyleSheet resolved(prepared, origin, registry_, media_);
    append_sheet(resolved, library);
}

void CompiledStyleSheet::append_sheet(const CompiledStyleSheet& sheet, bool library) {
    if (&sheet == this) {
        return; // a sheet merged into itself would read its own reallocated rule vector
    }

    // `tessera-ui` itself: the layer unlayered library rules land in and the parent of the
    // library's own named layers. It is created before any of them, so it always sorts below them.
    const std::uint32_t library_base = library ? layer_slot("tessera-ui", {}) : kUnlayeredLayer;

    // Slots are dense and assigned in order, so walking them in ascending order reproduces the
    // source sheet's layer order in the global table. A named layer merges by name (`@layer base`
    // in two sheets is one layer); an anonymous layer keeps its position under a fresh slot.
    std::uint32_t slot_count = 0;
    for (const auto& [name, slot] : sheet.layer_order_) {
        (void)name;
        slot_count = std::max(slot_count, slot + 1);
    }
    for (const CompiledRule& rule : sheet.rules_) {
        if (rule.layer != kUnlayeredLayer) {
            slot_count = std::max(slot_count, rule.layer + 1);
        }
    }
    std::vector<const std::string*> names(slot_count, nullptr);
    for (const auto& [name, slot] : sheet.layer_order_) {
        if (slot < slot_count) {
            names[slot] = &name;
        }
    }
    std::vector<std::uint32_t> remap(slot_count, kUnlayeredLayer);
    for (std::uint32_t slot = 0; slot < slot_count; ++slot) {
        const std::string* name = names[slot];
        if (name == nullptr) {
            remap[slot] = next_layer_++;
        } else if (!library || is_tessera_ui_layer(*name)) {
            // A library sheet may already write its CSS inside `@layer tessera-ui`: keep the name
            // instead of nesting it twice.
            remap[slot] = layer_slot(*name, {});
        } else {
            remap[slot] = layer_slot(*name, "tessera-ui");
        }
    }

    const std::uint64_t order_base = next_source_order_;
    const std::size_t rule_base = rules_.size();
    for (const CompiledRule& rule : sheet.rules_) {
        std::uint32_t layer = rule.layer;
        if (layer != kUnlayeredLayer) {
            layer = layer < remap.size() ? remap[layer] : kUnlayeredLayer;
        } else if (library) {
            layer = library_base;
        }
        rules_.push_back(CompiledRule{rule.declarations, order_base + rule.source_order, layer,
                                      rule.containers, rule.origin});
    }
    for (const SelectorIndex::Entry& entry : sheet.index_.entries()) {
        if (entry.value < sheet.rules_.size()) {
            index_.add(entry.selector, rule_base + entry.value);
        }
    }
    for (const KeyframeBlock& block : sheet.keyframes_) {
        // CSS keeps the first definition of a keyframes name in the cascade.
        if (!has_keyframes_named(keyframes_, block.name)) {
            keyframes_.push_back(block);
        }
    }
    container_queries_.insert(container_queries_.end(), sheet.container_queries_.begin(),
                              sheet.container_queries_.end());
    diagnostics_.insert(diagnostics_.end(), sheet.diagnostics_.begin(), sheet.diagnostics_.end());
    has_container_queries_ = has_container_queries_ || sheet.has_container_queries_;
    next_source_order_ = order_base + sheet.next_source_order_;
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
        cascade.add_declarations(rule.declarations, rule.origin, match.specificity, source_order,
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
