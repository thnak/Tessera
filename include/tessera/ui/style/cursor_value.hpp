// SPDX-License-Identifier: MIT

#pragma once

// Value grammar of the CSS `cursor` property (issue #191; the normative keyword list is
// docs/specs/window-chrome-v1.md section 8.5). One source for the three consumers, exactly like
// `interaction_values.hpp` does for the interaction properties: the declaration validator
// (property_value_validation.cpp), `@supports` (supports.cpp) and the computed-style -> scene sync
// (interaction_style.cpp).
//
// The grammar is the CSS cursor keyword set V1 maps to a native pointer. `url()` cursor lists are
// recognized but not implemented: they are reported once as `StyleDiagnosticKind::UnsupportedValue`
// and the declaration falls back to the initial `auto`. CSS-wide keywords (`inherit`, `initial`,
// ...) and `var()` are the cascade's business and are never a value-form diagnostic here.
//
// This header stays UI-layer only: the scene stores a `CursorKind`, and the app maps it onto the
// HAL's `hal::CursorShape`. No `hal::` header is included from `ui/` (ADR-020).
//
// Kept deliberately light: only the enum and the pure grammar, with `StyleDiagnostic` forward-
// declared, so `ui/scene/node.hpp` can store a `CursorKind` without pulling the cascade in (the
// same reason `pseudo_state.hpp` exists). The validator/`@supports` consumers include the full
// cascade.

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace tessera::ui::style {

struct StyleDiagnostic;

// The CSS cursor keywords V1 maps to a native pointer. `Auto` is the initial value and resolves to
// the platform default arrow (never "whatever the OS thinks", so a result is deterministic). The
// order is the canonical keyword order; the app's `CursorKind -> hal::CursorShape` switch is
// exhaustive over it.
enum class CursorKind : std::uint8_t {
    Auto,
    Default,
    None,
    ContextMenu,
    Help,
    Pointer,
    Progress,
    Wait,
    Cell,
    Crosshair,
    Text,
    VerticalText,
    Alias,
    Copy,
    Move,
    NoDrop,
    NotAllowed,
    Grab,
    Grabbing,
    AllScroll,
    ZoomIn,
    ZoomOut,
    ResizeN,
    ResizeE,
    ResizeS,
    ResizeW,
    ResizeNe,
    ResizeNw,
    ResizeSe,
    ResizeSw,
    ResizeNs,
    ResizeEw,
    ResizeNesw,
    ResizeNwse,
    ColResize,
    RowResize,
};

// True when `value` is a single bare keyword form: one identifier, trimmed and case-insensitive,
// with no function call or comma list. The declaration validator uses it to tell "a keyword V1 may
// or may not implement" from "a cursor list" (`url(...)`/`<image>`), which V1 rejects outright.
[[nodiscard]] bool is_cursor_keyword(std::string_view value) noexcept;

// Case-insensitive and trimmed. True when `value` is one of the supported keywords above; false for
// a `url(...)` cursor list and for an unknown keyword. `initial`/`inherit`/`unset` are the
// cascade's business and are not judged here (the caller checks them first).
[[nodiscard]] bool is_supported_cursor_value(std::string_view value) noexcept;

// Case-insensitive and trimmed. An unrecognised keyword returns `CursorKind::Auto`, the CSS
// fallback policy the interaction properties use too: a value V1 did not implement never reaches a
// consumer as something it cannot map.
[[nodiscard]] CursorKind parse_cursor_kind(std::string_view value) noexcept;

// The canonical CSS keyword of a kind ("auto", "context-menu", "resize-nwse", ...), for diagnostics
// and tests. Never returns nullptr; `parse_cursor_kind(cursor_kind_name(kind)) == kind` for every
// kind.
[[nodiscard]] std::string_view cursor_kind_name(CursorKind kind) noexcept;

// Appends exactly one `StyleDiagnosticKind::UnsupportedValue` when `value` is a cursor form V1 does
// not implement: a `url(...)`/`<image>` cursor list or an unknown keyword. A supported keyword, a
// CSS-wide keyword (`inherit`, `initial`, ...) and a `var()` reference are not judged here. The
// declaration itself is kept by the caller, so the AST stays complete and the computed value falls
// back to `auto` (section 11's rule: unsupported value forms are reported, never silent).
void report_unsupported_cursor_value(std::string_view value, std::size_t offset,
                                     std::vector<StyleDiagnostic>& out);

} // namespace tessera::ui::style
