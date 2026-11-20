// SPDX-License-Identifier: MIT

#include "selector_text.hpp"
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

// Whether `node` sits inside `region` (issue #622): some ancestor (or the node itself) matches
// `start`, and no ancestor between that one and the node matches `end`. The nearest matching start
// wins, which is what makes a nested `@scope` a region rather than a union of every ancestor.
bool in_scope(const ScopeRegion& region, const StyleNode& node) {
    const StyleNode* start = nullptr;
    for (const StyleNode* current = &node; current != nullptr; current = current->parent) {
        if (matches(region.start, *current)) {
            start = current;
            break;
        }
    }
    if (start == nullptr) {
        return false;
    }
    if (!region.has_end) {
        return true;
    }
    // Walk from the node up to (but not including) the start element: an `end` in between closes
    // the region before the node.
    for (const StyleNode* current = &node; current != nullptr && current != start;
         current = current->parent) {
        if (matches(region.end, *current)) {
            return false;
        }
    }
    return true;
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
            // The rules this source contributes are the tail of `rules_`; stamping the path here is
            // what lets `style.info` name the file a rule's offset is an offset into, without
            // threading a path through `append_sheet`'s signature.
            const std::size_t first = rules_.size();
            append_sheet(*source.prepared, source.origin, source.library);
            for (std::size_t index = first; index < rules_.size(); ++index) {
                rules_[index].source_path = source.path;
            }
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
            // The authored selector text is carried when the sheet was prepared from source; a
            // blob-loaded sheet has none, so it is printed back from the parsed list. Either way
            // `style.info` names the rule a developer would recognise.
            std::string selector = item.selector;
            if (selector.empty()) {
                selector = selector_list_text(item.selectors);
            }
            rules_.push_back(CompiledRule{.declarations = item.declarations,
                                          .source_order = next_source_order_,
                                          .layer = layer,
                                          .containers = item.containers,
                                          .scopes = item.scopes,
                                          .origin = origin_,
                                          .selector = std::move(selector),
                                          .offset = item.offset,
                                          .source_path = {}});
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
        rules_.push_back(CompiledRule{.declarations = rule.declarations,
                                      .source_order = order_base + rule.source_order,
                                      .layer = layer,
                                      .containers = rule.containers,
                                      .scopes = rule.scopes,
                                      .origin = rule.origin,
                                      .selector = rule.selector,
                                      .offset = rule.offset,
                                      .source_path = rule.source_path});
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

std::vector<CompiledStyleSheet::MatchedRule>
CompiledStyleSheet::matched_rules(const StyleNode& node,
                                  const ContainerSizeSource* containers) const {
    std::vector<MatchedRule> matched;
    // The selector matches go into a reused buffer, so a whole-tree style pass does not allocate
    // one vector per node (security review finding P3).
    index_.match(node, match_scratch_);
    for (const SelectorMatch& match : match_scratch_) {
        if (match.value >= rules_.size()) {
            continue;
        }
        const CompiledRule& rule = rules_[match.value];
        // The user-agent sheet describes the built-in elements. A component whose name happens to
        // be `Button` or `Text` is the author's own element, so a bare `Button { ... }` default
        // must not style its host box (it drew a grey frame around the gallery's buttons).
        if (rule.origin == Origin::UserAgent && node.component_host) {
            continue;
        }
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
        // `@scope` is a filter on top of the selector (issue #622): the rule applies only when the
        // node is inside every enclosing region. It adds no cascade key component, so a scoped rule
        // still resolves by specificity and then source order like an unscoped one.
        if (!std::all_of(rule.scopes.begin(), rule.scopes.end(),
                         [&](const ScopeRegion& region) { return in_scope(region, node); })) {
            continue;
        }
        matched.push_back(MatchedRule{&rule, match.specificity});
    }
    return matched;
}

void CompiledStyleSheet::add_to_cascade(const StyleNode& node, CascadeTable& cascade,
                                        const ContainerSizeSource* containers) const {
    // One implementation of "which rules apply": the inspector reads the same list, so a rule it
    // names is a rule the cascade used.
    for (const MatchedRule& match : matched_rules(node, containers)) {
        std::uint64_t source_order = match.rule->source_order;
        cascade.add_declarations(match.rule->declarations, match.rule->origin, match.specificity,
                                 source_order, match.rule->layer);
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
