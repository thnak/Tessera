// SPDX-License-Identifier: MIT
//
// The property table: every longhand the cascade knows, with its initial value and whether it
// inherits. Split out of property_registry.cpp, which is at the 500-line hard limit and whose job
// is the registry's behaviour (lookup, shorthand expansion, value validation), not the data.
//
// The table is the one place a property is declared. `docs/specs/stylesheet-v1.properties.txt`
// lists the same names by row and `property_registry_matrix` fails when the two disagree, so adding
// a property means adding a row here and a line there.

#pragma once

namespace tessera::ui::style {

struct PropertyDef {
    const char* name;
    const char* initial;
    bool inherited;
};

constexpr PropertyDef kProperties[] = {
    {"display", "inline", false},
    {"position", "static", false},
    {"box-sizing", "content-box", false},
    {"width", "auto", false},
    {"height", "auto", false},
    // Ratio of width to height (issue #247): per box, not inherited, read by the layout adapter.
    {"aspect-ratio", "auto", false},
    {"min-width", "auto", false},
    {"min-height", "auto", false},
    {"max-width", "none", false},
    {"max-height", "none", false},
    {"top", "auto", false},
    {"right", "auto", false},
    {"bottom", "auto", false},
    {"left", "auto", false},
    {"margin-top", "0", false},
    {"margin-right", "0", false},
    {"margin-bottom", "0", false},
    {"margin-left", "0", false},
    {"padding-top", "0", false},
    {"padding-right", "0", false},
    {"padding-bottom", "0", false},
    {"padding-left", "0", false},
    {"border-top-width", "0", false},
    {"border-right-width", "0", false},
    {"border-bottom-width", "0", false},
    {"border-left-width", "0", false},
    {"border-top-style", "none", false},
    {"border-right-style", "none", false},
    {"border-bottom-style", "none", false},
    {"border-left-style", "none", false},
    {"border-top-color", "currentColor", false},
    {"border-right-color", "currentColor", false},
    {"border-bottom-color", "currentColor", false},
    {"border-left-color", "currentColor", false},
    {"border-top-left-radius", "0", false},
    {"border-top-right-radius", "0", false},
    {"border-bottom-right-radius", "0", false},
    {"border-bottom-left-radius", "0", false},
    {"color", "black", true},
    {"background-color", "transparent", false},
    {"background-image", "none", false},
    // Same grammar as `background-image` (a list of gradients, painted bottom to top); the paint
    // pass draws it as the alpha mask of an isolated layer (issue #617, offscreen-layers.md 5.2).
    {"mask-image", "none", false},
    // Per-layer geometry of the mask image list (mask_layers.hpp); a short list repeats cyclically.
    {"mask-size", "auto", false},
    {"mask-position", "0% 0%", false},
    {"mask-repeat", "repeat", false},
    {"mask-mode", "match-source", false},
    // `none` or a list of blur() and drop-shadow() applied to an isolated layer (issue #617).
    {"filter", "none", false},
    {"opacity", "1", false},
    {"font-family", "sans-serif", true},
    {"font-size", "16px", true},
    {"font-style", "normal", true},
    {"font-weight", "400", true},
    {"line-height", "normal", true},
    {"letter-spacing", "normal", true},
    {"word-spacing", "normal", true},
    {"text-align", "start", true},
    {"text-transform", "none", true},
    // Not inherited (CSS): the box that paints the line owns truncation (docs/design/text.md 6.3).
    {"text-overflow", "clip", false},
    {"white-space", "normal", true},
    {"visibility", "visible", true},
    {"cursor", "auto", true},
    {"color-scheme", "normal", true},
    // Interaction (issue #247): inherited; the scene reads both through sync_interaction_style.
    {"pointer-events", "auto", true},
    {"user-select", "auto", true},
    {"direction", "ltr", true},
    {"writing-mode", "horizontal-tb", true},
    {"flex-direction", "row", false},
    {"flex-wrap", "nowrap", false},
    {"flex-grow", "0", false},
    {"flex-shrink", "1", false},
    {"flex-basis", "auto", false},
    {"align-items", "stretch", false},
    {"align-self", "auto", false},
    {"align-content", "normal", false},
    {"justify-content", "normal", false},
    {"justify-items", "legacy", false},
    {"row-gap", "normal", false},
    {"column-gap", "normal", false},
    // CSS Grid (issue #249, docs/specs/grid-v1.md): `grid-column`/`grid-row` hold the whole
    // `<start> / <end>` value; V1 has no `-start`/`-end` longhands.
    {"grid-template-columns", "none", false},
    {"grid-template-rows", "none", false},
    {"grid-column", "auto", false},
    {"grid-row", "auto", false},
    {"grid-auto-flow", "row", false},
    {"justify-self", "auto", false},
    // `clip-path` basic shapes (ADR-068 slice 4, issue #672). The initial value is `none`, which is
    // what every element had before the property existed, so no existing scene changes. Not
    // inherited: a clip applies to the element that declares it and its subtree, which the paint
    // pass handles by pushing the clip around the subtree rather than by inheritance.
    {"clip-path", "none", false},
    {"overflow-x", "visible", false},
    {"overflow-y", "visible", false},
    // Scroll runtime/style inputs (stylesheet-v1 section 11, scroll-and-gestures-v1 section 4).
    // None is inherited: scroll policy belongs to the container that owns the scrollport.
    // `overscroll-effect` has no `auto` keyword; the initial request is `platform`.
    {"scroll-behavior", "auto", false},
    {"overscroll-behavior", "auto", false},
    {"overscroll-effect", "platform", false},
    {"scrollbar-gutter", "auto", false},
    {"order", "0", false},
    {"field-sizing", "fixed", false},
    {"container-type", "normal", false},
    {"container-name", "none", false},
    {"z-index", "auto", false},
    {"transform", "none", false},
    {"transform-origin", "50% 50%", false},
    {"box-shadow", "none", false},
    // The OS backdrop of the root window (glass-v1.md section 6): an ordinary cascade property so a
    // theme can set it; only the window's root reads it, and the platform resolves `auto`.
    {"window-backdrop", "none", false},
    // Pasted-web-CSS alias of `backdrop-filter` (glass-v1 section 2); one winner per node.
    {"backdrop-filter", "none", false},
    {"-webkit-backdrop-filter", "none", false},
};

} // namespace tessera::ui::style
