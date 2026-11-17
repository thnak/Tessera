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

// One entry in source order. Only the fields of `kind` are meaningful.
struct PreparedItem {
    PreparedItemKind kind = PreparedItemKind::Rule;
    // Enclosing `@media` queries; the item applies when all of them match.
    std::vector<MediaQueryList> media;

    // Rule
    SelectorList selectors;
    std::vector<Declaration> declarations; // shorthands already expanded
    std::uint32_t layer = kUnlayeredLayer; // index into PreparedStyleSheet::layers
    std::vector<ContainerQuery> containers;

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
