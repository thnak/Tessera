// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/selector.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tessera::ui::style {

enum class PseudoState : std::uint32_t {
    None = 0,
    Hover = 1u << 0,
    Active = 1u << 1,
    Focus = 1u << 2,
    FocusVisible = 1u << 3,
    Disabled = 1u << 4,
    Checked = 1u << 5,
    Required = 1u << 6,
};

constexpr PseudoState operator|(PseudoState lhs, PseudoState rhs) {
    return static_cast<PseudoState>(static_cast<std::uint32_t>(lhs) |
                                    static_cast<std::uint32_t>(rhs));
}

constexpr PseudoState operator&(PseudoState lhs, PseudoState rhs) {
    return static_cast<PseudoState>(static_cast<std::uint32_t>(lhs) &
                                    static_cast<std::uint32_t>(rhs));
}

constexpr bool has_state(PseudoState value, PseudoState flag) {
    return (value & flag) != PseudoState::None;
}

struct AttributeView {
    std::string_view name;
    std::string_view value;
};

struct StyleNode {
    std::string_view tag;
    std::string_view id;
    std::vector<std::string_view> classes;
    std::vector<AttributeView> attributes;
    std::vector<StyleNode*> children;
    StyleNode* parent = nullptr;
    PseudoState state = PseudoState::None;
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
    void add(const ComplexSelector& selector, std::size_t value);
    void add(const SelectorList& selectors, std::size_t value);
    void clear();

    [[nodiscard]] std::vector<SelectorMatch> match(const StyleNode& node) const;
    [[nodiscard]] std::size_t size() const {
        return entries_.size();
    }

private:
    struct Entry {
        ComplexSelector selector;
        std::size_t value = 0;
        Specificity specificity;
    };

    void add_entry(Entry entry);

    std::vector<Entry> entries_;
    std::unordered_map<std::string, std::vector<std::size_t>> id_index_;
    std::unordered_map<std::string, std::vector<std::size_t>> class_index_;
    std::unordered_map<std::string, std::vector<std::size_t>> tag_index_;
    std::vector<std::size_t> universal_index_;
};

} // namespace tessera::ui::style
