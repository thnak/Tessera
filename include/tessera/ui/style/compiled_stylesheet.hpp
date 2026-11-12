// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/cascade.hpp"
#include "tessera/ui/style/container_query.hpp"
#include "tessera/ui/style/keyframes.hpp"
#include "tessera/ui/style/media_query.hpp"
#include "tessera/ui/style/parser.hpp"
#include "tessera/ui/style/prepared_stylesheet.hpp"
#include "tessera/ui/style/property_registry.hpp"
#include "tessera/ui/style/selector_match.hpp"

#include <cstdint>
#include <map>
#include <span>
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
    // Cascade origin of this rule. A single-source sheet has one origin for all its rules; a
    // combination carries the origin of the sheet each rule came from.
    Origin origin = Origin::Author;
};

// One input of an ordered multi-sheet cascade (ADR-048 decisions 5-6;
// docs/specs/compiled-stylesheet-v1.md section 10). `path` only names the sheet in diagnostics.
// `library` marks first-party library CSS: every rule of that sheet lands in `@layer tessera-ui`
// (its named layers nested under it, `@layer base` -> `tessera-ui.base`), so unlayered app CSS wins
// regardless of specificity. The cascade keeps working unchanged for single-sheet callers, which
// never set it.
struct StylesheetSource {
    std::string_view path;
    const PreparedStyleSheet* prepared = nullptr;
    Origin origin = Origin::Author;
    bool library = false;
};

class CompiledStyleSheet {
public:
    explicit CompiledStyleSheet(StyleSheet sheet, Origin origin = Origin::Author,
                                PropertyRegistry registry = PropertyRegistry{},
                                MediaContext media = MediaContext{});
    // Resolve stage (ADR-048): evaluates `@media` against `media` and builds the cascade
    // structures from an already prepared sheet, without tokenizing or parsing.
    explicit CompiledStyleSheet(const PreparedStyleSheet& prepared, Origin origin = Origin::Author,
                                PropertyRegistry registry = PropertyRegistry{},
                                MediaContext media = MediaContext{});
    // Ordered combination (ADR-048 decisions 5-6; docs/specs/compiled-stylesheet-v1.md section 10):
    // resolves every source in one cascade. Rules are concatenated with a global, monotonically
    // increasing `source_order` in source order, so for an equal
    // origin/importance/layer/specificity key the later sheet wins (the web import-graph rule).
    // Layer names are remapped into one global table (`@layer base` in two sheets is one layer),
    // keyframes keep the first definition of a name, and container queries and diagnostics
    // concatenate in source order. Library sources are combined before app sources so `@layer
    // tessera-ui` (and its nested layers) always sorts below every app layer; within each group the
    // given order is kept.
    explicit CompiledStyleSheet(std::span<const StylesheetSource> sheets,
                                PropertyRegistry registry = PropertyRegistry{},
                                MediaContext media = MediaContext{});

    // Appends one prepared sheet to this cascade with a single global source order. Registration
    // uses the compiled overload below; both keep the cascade in one place.
    void append_sheet(const PreparedStyleSheet& prepared, Origin origin = Origin::Author,
                      bool library = false);
    // Appends one already resolved sheet: its rules continue this sheet's source order, its layer
    // names merge into this sheet's layer table, and a repeated keyframes name is dropped (the
    // first definition wins). Used by `AppHost` to rebuild a cascade from its constructor sheet
    // plus the app's embedded sheets.
    void append_sheet(const CompiledStyleSheet& sheet, bool library = false);

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
    // The sheet's origin. A combination reports the last source's origin; the authoritative origin
    // of a rule is `CompiledRule::origin`, which `add_to_cascade` uses.
    [[nodiscard]] Origin origin() const {
        return origin_;
    }

private:
    void resolve(const PreparedStyleSheet& prepared);
    // Global slot of `name` (optionally namespaced by `prefix`), assigned on first use.
    [[nodiscard]] std::uint32_t layer_slot(std::string_view name, std::string_view prefix);

    Origin origin_ = Origin::Author;
    PropertyRegistry registry_;
    MediaContext media_;
    std::vector<CompiledRule> rules_;
    SelectorIndex index_;
    std::vector<KeyframeBlock> keyframes_;
    std::vector<StyleDiagnostic> diagnostics_;
    std::map<std::string, std::uint32_t> layer_order_;
    std::vector<ContainerQuery> container_queries_;
    bool has_container_queries_ = false;
    // Next free global layer slot and source order, so an appended sheet continues the cascade.
    std::uint32_t next_layer_ = 0;
    std::uint64_t next_source_order_ = 0;
};

CompiledStyleSheet compile_stylesheet(std::string_view source, Origin origin = Origin::Author,
                                      PropertyRegistry registry = PropertyRegistry{},
                                      MediaContext media = MediaContext{});

} // namespace tessera::ui::style
