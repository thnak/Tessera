// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/pseudo_state.hpp"
#include "tessera/ui/style/selector.hpp"

#include <cstddef>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tessera::ui::style {

struct AttributeView {
    std::string_view name;
    std::string_view value;
};

struct StyleNode {
    std::string_view tag;
    std::string_view id;
    // Whitespace-separated `part` attribute names: the styling API an element exposes to its host
    // (stylesheet-v1 section 6.3). Empty for an element that is no part.
    std::string_view part;
    std::vector<std::string_view> classes;
    std::vector<AttributeView> attributes;
    std::vector<StyleNode*> children;
    StyleNode* parent = nullptr;
    // The nearest ancestor component instance: the `::part()` boundary. A part outside a component
    // subtree is not a part, so `::part()` never matches a node whose `host` is null. The view is
    // precomputed when the tree is captured, so matching stays allocation-free.
    StyleNode* host = nullptr;
    PseudoState state = PseudoState::None;
    // True when this node is itself a component instance host (its `tag` is the component name).
    bool component_host = false;
    bool has_text = false;
};

struct SelectorMatch {
    std::size_t value = 0;
    Specificity specificity;
};

bool matches(const CompoundSelector& selector, const StyleNode& node);
bool matches(const ComplexSelector& selector, const StyleNode& node);
bool matches(const SelectorList& selectors, const StyleNode& node);

class SelectorIndex {
public:
    struct Entry {
        ComplexSelector selector;
        std::size_t value = 0;
        Specificity specificity;
    };

    void add(const ComplexSelector& selector, std::size_t value);
    void add(const SelectorList& selectors, std::size_t value);
    void clear();

    [[nodiscard]] std::vector<SelectorMatch> match(const StyleNode& node) const;
    [[nodiscard]] std::size_t size() const {
        return entries_.size();
    }
    // The indexed selectors in insertion order; `value` is the caller's rule handle. A combination
    // re-adds these entries against the shifted rule indices of its own sheet.
    [[nodiscard]] const std::vector<Entry>& entries() const {
        return entries_;
    }

private:
    void add_entry(Entry entry);

    std::vector<Entry> entries_;
    std::unordered_map<std::string, std::vector<std::size_t>> id_index_;
    std::unordered_map<std::string, std::vector<std::size_t>> class_index_;
    std::unordered_map<std::string, std::vector<std::size_t>> tag_index_;
    // Selectors whose subject compound carries `::part()`: bucketed by the first part name, because
    // the compound's tag/class/id belong to the host and can never be read off the part element.
    std::unordered_map<std::string, std::vector<std::size_t>> part_index_;
    std::vector<std::size_t> universal_index_;
};

} // namespace tessera::ui::style
