// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/cascade.hpp"
#include "tessera/ui/style/container_query.hpp"
#include "tessera/ui/style/keyframes.hpp"
#include "tessera/ui/style/media_query.hpp"
#include "tessera/ui/style/parser.hpp"
#include "tessera/ui/style/property_registry.hpp"
#include "tessera/ui/style/selector.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::style {

// The media-independent stage of stylesheet compilation (ADR-048 decision 2). Prepare parses
// selectors, expands shorthands, parses at-rule blocks, evaluates `@supports` and fixes layer and
// source order, but never evaluates `@media`: every item carries the media queries that enclose it
// and `CompiledStyleSheet` resolves them against a `MediaContext`. The blob format of
// docs/specs/compiled-stylesheet-v1.md serializes exactly this structure.
enum class PreparedItemKind : std::uint8_t {
    Rule,
    Layer,
    Container,
    Keyframes,
};

// One `@scope (<start>) [to (<end>)]` block a rule sits inside (issue #622). A rule applies only
// when the node is inside the scope: some ancestor (or the node itself) matches `start`, and no
// ancestor between that one and the node matches `end`. An empty `end` runs to the end of the tree.
// The scope is a filter on top of the rule's own selector, not a replacement for it.
struct ScopeRegion {
    SelectorList start;
    SelectorList end;
    bool has_end = false;
};

// One entry in source order. Only the fields of `kind` are meaningful.
struct PreparedItem {
    PreparedItemKind kind = PreparedItemKind::Rule;
    // Enclosing `@media` queries; the item applies when all of them match.
    std::vector<MediaQueryList> media;

    // Rule
    SelectorList selectors;
    // The selector list as the author wrote it, and the byte offset of the rule in its sheet. Both
    // exist for the designer's `style.info` (designer-protocol-v1 section 9.1), which has to name
    // the rule a developer would recognise and point at it in their file.
    //
    // **Why they are not in the blob** (docs/specs/compiled-stylesheet-v1.md section 5): the text
    // is reconstructible from `selectors` and the offset is meaningless in a build artifact, which
    // has no source file to point into. Carrying them would bump `format_version` and regenerate
    // every committed blob golden for a field only a design-time query reads, so a blob-loaded
    // sheet leaves `selector` empty and `offset` 0 and the resolve step prints the selector back.
    std::string selector;
    std::size_t offset = 0;
    std::vector<Declaration> declarations; // shorthands already expanded
    std::uint32_t layer = kUnlayeredLayer; // index into PreparedStyleSheet::layers
    std::vector<ContainerQuery> containers;
    // Enclosing `@scope` regions, outermost first. All of them must hold (nesting is a
    // conjunction).
    std::vector<ScopeRegion> scopes;

    // Layer: every layer the at-rule names or creates, in order of first appearance.
    std::vector<std::uint32_t> layer_refs;

    // Container: the query of an `@container` block, registered for the container-query tracker.
    ContainerQuery container;

    // Keyframes
    KeyframeBlock keyframes;
};

struct PreparedStyleSheet {
    // Content hash of the inputs (FNV-1a 64 of the CSS text); 0 when prepared from a parsed tree.
    std::uint64_t source_hash = 0;
    // Layer names by first use; an empty name is an anonymous layer.
    std::vector<std::string> layers;
    std::vector<PreparedItem> items;
    // Media-independent diagnostics (unsupported properties, invalid at-rules).
    std::vector<StyleDiagnostic> diagnostics;

    // True when resolving this sheet against `before` and against `after` can produce a different
    // cascade (issue #436). A window resize asks this before it re-resolves anything, so a
    // drag-resize that crosses no breakpoint stays a layout pass.
    //
    // **Why the answer is exact rather than a breakpoint list.** A rule participates or not
    // depending on `evaluate(list, context)` for each enclosing `@media` (at-rule blocks sit in
    // `PreparedItem::media`), and that boolean is a pure function of the context. So comparing the
    // booleans is the same question as "did a breakpoint get crossed", answered without parsing a
    // length, special-casing `orientation` or guessing at `vw`/`vh`: a unit-, negation- or
    // comma-list-shaped query is handled by the same evaluation the cascade itself would run. A
    // threshold list can only ever be a superset of this answer, and a superset that is wrong in
    // the common case is exactly the "rebuild the cascade on every pixel" bug this exists to stop.
    [[nodiscard]] bool media_answer_can_change(const MediaContext& before,
                                               const MediaContext& after) const;
};

[[nodiscard]] std::uint64_t fnv1a64(std::string_view bytes);

PreparedStyleSheet prepare_stylesheet(const StyleSheet& sheet,
                                      const PropertyRegistry& registry = PropertyRegistry{});
PreparedStyleSheet prepare_stylesheet(std::string_view source,
                                      const PropertyRegistry& registry = PropertyRegistry{});

} // namespace tessera::ui::style
