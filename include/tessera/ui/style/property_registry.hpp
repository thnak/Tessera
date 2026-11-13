// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/cascade.hpp"
#include "tessera/ui/style/parser.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::style {

struct PropertyInfo {
    std::string initial;
    bool inherited = false;
};

struct PropertyExpansionResult {
    std::vector<Declaration> declarations;
    std::vector<StyleDiagnostic> diagnostics;
};

class PropertyRegistry {
public:
    PropertyRegistry();

    [[nodiscard]] const PropertyInfo* find(std::string_view property) const;
    [[nodiscard]] bool supported(std::string_view property) const;
    [[nodiscard]] const std::vector<std::string>* longhands(std::string_view shorthand) const;

    // Every registered longhand name, sorted by byte value (the registry's map order). Two
    // tests walk this list: the per-property status table in
    // docs/specs/stylesheet-property-status-v1.md (issue #213) and the V1 compatibility matrix in
    // docs/specs/stylesheet-v1.properties.txt (issue #250), so a property cannot be registered
    // without a recorded consumer or a matrix row. Views stay valid for the registry's lifetime.
    [[nodiscard]] std::vector<std::string_view> longhand_names() const;
    [[nodiscard]] std::vector<std::string_view> shorthand_names() const;

    [[nodiscard]] PropertyExpansionResult expand(const Declaration& declaration) const;
    [[nodiscard]] PropertyExpansionResult
    expand(const std::vector<Declaration>& declarations) const;

    [[nodiscard]] ComputedStyle initial_style() const;
    void apply_initial_values(ComputedStyle& style) const;
    [[nodiscard]] ComputedStyle compute_with_initial(const ComputedStyle* parent,
                                                     const CascadeTable& cascade) const;

private:
    enum class ShorthandKind : std::uint8_t {
        Box4,
        Pair,
        Flex,
        // One logical border side (`border-inline-start`/`border-inline-end`): the CSS
        // `<width> || <style> || <color>` grammar, expanded to that side's three longhands.
        BorderSide,
    };

    struct ShorthandInfo {
        std::vector<std::string> longhands;
        ShorthandKind kind = ShorthandKind::Box4;
    };

    [[nodiscard]] PropertyExpansionResult expand_box(const Declaration& declaration,
                                                     const ShorthandInfo& shorthand,
                                                     std::string_view property) const;
    [[nodiscard]] PropertyExpansionResult expand_pair(const Declaration& declaration,
                                                      const ShorthandInfo& shorthand,
                                                      std::string_view property) const;
    [[nodiscard]] PropertyExpansionResult expand_flex(const Declaration& declaration) const;
    [[nodiscard]] PropertyExpansionResult expand_border_side(const Declaration& declaration,
                                                             const ShorthandInfo& shorthand,
                                                             std::string_view property) const;

    // Registers the inline-axis (direction-relative) longhands and shorthands (issue #230). Defined
    // in `logical_properties.cpp` so this table and `expand` stay under the source line limit.
    void register_logical_properties();

    // Registers the `border` shorthand and the animation/transition names (issue #250). Defined in
    // `property_registry_extra.cpp` for the same reason: the animation and transition grammars live
    // in `src/ui/animation/`, and the registry only has to accept the names so the cascade stores
    // them and `@supports` is correct for them.
    void register_extension_properties();

    std::map<std::string, PropertyInfo> properties_;
    std::map<std::string, ShorthandInfo> shorthands_;
};

} // namespace tessera::ui::style
