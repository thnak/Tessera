// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/cascade.hpp"
#include "tessera/ui/style/parser.hpp"
#include "tessera/ui/style/property_registry.hpp"
#include "tessera/ui/style/selector_match.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace tessera::ui::style {

struct CompiledRule {
    std::vector<Declaration> declarations;
    std::uint64_t source_order = 0;
};

class CompiledStyleSheet {
public:
    explicit CompiledStyleSheet(StyleSheet sheet, Origin origin = Origin::Author,
                                PropertyRegistry registry = PropertyRegistry{});

    void add_to_cascade(const StyleNode& node, CascadeTable& cascade) const;
    [[nodiscard]] CascadeTable cascade(const StyleNode& node) const;
    [[nodiscard]] ComputedStyle compute(const StyleNode& node, const ComputedStyle* parent) const;

    [[nodiscard]] const std::vector<CompiledRule>& rules() const {
        return rules_;
    }
    [[nodiscard]] const SelectorIndex& index() const {
        return index_;
    }
    [[nodiscard]] const std::vector<StyleDiagnostic>& diagnostics() const {
        return diagnostics_;
    }
    [[nodiscard]] const PropertyRegistry& registry() const {
        return registry_;
    }
    [[nodiscard]] Origin origin() const {
        return origin_;
    }

private:
    Origin origin_ = Origin::Author;
    PropertyRegistry registry_;
    std::vector<CompiledRule> rules_;
    SelectorIndex index_;
    std::vector<StyleDiagnostic> diagnostics_;
};

CompiledStyleSheet compile_stylesheet(std::string_view source, Origin origin = Origin::Author,
                                      PropertyRegistry registry = PropertyRegistry{});

} // namespace tessera::ui::style
