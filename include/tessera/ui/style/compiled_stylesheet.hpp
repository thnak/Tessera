// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/cascade.hpp"
#include "tessera/ui/style/container_query.hpp"
#include "tessera/ui/style/keyframes.hpp"
#include "tessera/ui/style/media_query.hpp"
#include "tessera/ui/style/parser.hpp"
#include "tessera/ui/style/property_registry.hpp"
#include "tessera/ui/style/selector_match.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::style {

struct CompiledRule {
    std::vector<Declaration> declarations;
    std::uint64_t source_order = 0;
    std::uint32_t layer = kUnlayeredLayer;
    // Enclosing `@container` queries, all of which must hold (nesting is a conjunction).
    std::vector<ContainerQuery> containers;
};

class CompiledStyleSheet {
public:
    explicit CompiledStyleSheet(StyleSheet sheet, Origin origin = Origin::Author,
                                PropertyRegistry registry = PropertyRegistry{},
                                MediaContext media = MediaContext{});

    // `containers` resolves `@container` rules; with null, rules inside `@container` never apply.
    void add_to_cascade(const StyleNode& node, CascadeTable& cascade,
                        const ContainerSizeSource* containers = nullptr) const;
    [[nodiscard]] CascadeTable cascade(const StyleNode& node,
                                       const ContainerSizeSource* containers = nullptr) const;
    [[nodiscard]] ComputedStyle compute(const StyleNode& node, const ComputedStyle* parent,
                                        const ContainerSizeSource* containers = nullptr) const;
    // Every `@container` query of the sheet, in source order, for the container-query tracker.
    [[nodiscard]] const std::vector<ContainerQuery>& container_queries() const {
        return container_queries_;
    }
    // True when any rule sits inside an `@container` block.
    [[nodiscard]] bool has_container_queries() const {
        return has_container_queries_;
    }

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
    [[nodiscard]] const MediaContext& media() const {
        return media_;
    }
    [[nodiscard]] const std::vector<KeyframeBlock>& keyframes() const {
        return keyframes_;
    }
    [[nodiscard]] const std::map<std::string, std::uint32_t>& layer_order() const {
        return layer_order_;
    }
    [[nodiscard]] Origin origin() const {
        return origin_;
    }

private:
    void compile_rules(const StyleSheet& sheet, std::uint64_t& source_order, std::uint32_t layer);
    void compile_rule(const Rule& rule, std::uint64_t& source_order, std::uint32_t layer);
    void compile_at_rule(const AtRule& at_rule, std::uint64_t& source_order, std::uint32_t layer);
    std::uint32_t layer_for(std::string_view name);

    Origin origin_ = Origin::Author;
    PropertyRegistry registry_;
    MediaContext media_;
    std::vector<CompiledRule> rules_;
    SelectorIndex index_;
    std::vector<KeyframeBlock> keyframes_;
    std::vector<StyleDiagnostic> diagnostics_;
    std::map<std::string, std::uint32_t> layer_order_;
    std::uint32_t next_layer_ = 0;
    std::vector<ContainerQuery> active_containers_;
    std::vector<ContainerQuery> container_queries_;
    bool has_container_queries_ = false;
};

CompiledStyleSheet compile_stylesheet(std::string_view source, Origin origin = Origin::Author,
                                      PropertyRegistry registry = PropertyRegistry{},
                                      MediaContext media = MediaContext{});

} // namespace tessera::ui::style
