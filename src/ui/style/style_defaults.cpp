// SPDX-License-Identifier: MIT
//
// The registry's initial values, shared by every computed style it computes (issue #735, slices E
// and F). See the header for why the table exists; this file is the lookup.

#include "tessera/ui/style/property_registry.hpp"
#include "tessera/ui/style/style_defaults.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::ui::style {
namespace {

// Open addressing needs a power-of-two capacity and a load factor below one half. The table is
// built once, so the probe count is not a build-time concern; only the read path is.
constexpr std::size_t kMinCapacity = 256;

std::size_t hash_name(std::string_view name) noexcept {
    // FNV-1a: the names are short and the table is built once, so a cheap mix is enough.
    std::uint64_t state = 14695981039346656037ULL;
    for (const char c : name) {
        state = (state ^ static_cast<std::uint8_t>(c)) * 1099511628211ULL;
    }
    return static_cast<std::size_t>(state);
}

} // namespace

StyleDefaults::StyleDefaults(std::vector<StyleDefault> definitions) {
    // Sorted-name order is the id order, so an id is stable across builds and a reader can compare
    // ids instead of names.
    std::sort(
        definitions.begin(), definitions.end(),
        [](const StyleDefault& left, const StyleDefault& right) { return left.name < right.name; });

    slots_.reserve(definitions.size());
    for (const StyleDefault& definition : definitions) {
        slots_.push_back(Slot{std::string(definition.name), std::string(definition.initial)});
    }

    std::size_t capacity = kMinCapacity;
    while (capacity < slots_.size() * 2) {
        capacity *= 2;
    }
    index_.assign(capacity, {std::string_view{}, kNoId});
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        const std::string_view name = slots_[i].name;
        std::size_t slot = hash_name(name) & (capacity - 1);
        while (index_[slot].second != kNoId) {
            slot = (slot + 1) & (capacity - 1);
        }
        index_[slot] = {name, static_cast<std::uint16_t>(i)};
    }
}

std::uint16_t StyleDefaults::id(std::string_view name) const {
    if (index_.empty()) {
        return kNoId;
    }
    const std::size_t mask = index_.size() - 1;
    std::size_t slot = hash_name(name) & mask;
    for (;;) {
        const auto& [key, value] = index_[slot];
        if (value == kNoId) {
            return kNoId;
        }
        if (key == name) {
            return value;
        }
        slot = (slot + 1) & mask;
    }
}

std::string_view StyleDefaults::name_of(std::uint16_t id) const {
    return id < slots_.size() ? slots_[id].name : std::string_view{};
}

const std::string* StyleDefaults::initial_of(std::uint16_t id) const {
    if (id >= slots_.size()) {
        return nullptr;
    }
    const std::string& initial = slots_[id].initial;
    // An empty initial value means "absent until declared", not "the empty string": the animation
    // and transition names must not appear in a computed style until a rule sets them.
    return initial.empty() ? nullptr : &initial;
}

const std::string* StyleDefaults::find(std::string_view name) const {
    return initial_of(id(name));
}

void PropertyRegistry::build_defaults() {
    std::vector<StyleDefault> definitions;
    definitions.reserve(properties_.size());
    for (const auto& [name, info] : properties_) {
        definitions.push_back(StyleDefault{name, info.initial, info.inherited});
    }
    defaults_ = std::make_shared<const StyleDefaults>(std::move(definitions));
}

} // namespace tessera::ui::style
