// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/hal/window.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace tessera::hal {

// The three simulated fold postures of the design-time studio (docs/specs/designer-studio-v1.md
// section 6, "Fold posture (foldables)" -> `viewport.set` `foldPosture`; docs/specs/os-events-v1.md
// section 2 for the `FoldInfo` they resolve to).
//
// This is the ONE mapping from a posture name to a `FoldInfo`. The Wasm viewer, the native preview
// CLI and the MCP `set_viewport` tool all call `fold_info_for()`, so a posture cannot mean one
// thing in the studio and another in an agent's snapshot: the preview feeds the result into
// `WindowMetrics::fold` and from there into the same `resolve_safe_area()` arithmetic the app runs.
//
// The postures are the spec's, not a device's:
//   flat      fully open: `FoldState::Flat`, no hinge band, no occlusion.
//   book      half-opened with a **vertical** hinge (left/right logical panes).
//   tabletop  half-opened with a **horizontal** hinge (content above, controls below).
//
// Both half-opened postures are `FoldOcclusion::Full` and `separating`: the studio simulates a
// device whose hinge occludes, which is the interesting case for `SafeArea` (a non-occluding fold
// would leave the padding arithmetic with nothing to do). `flat` carries no hinge geometry, so it
// is the "round trip back to flat restores the original frame" state of the issue's browser check.
enum class FoldPosture : std::uint8_t { None, Flat, Book, Tabletop };

// The posture `text` names, or `nullopt` for anything else (including the empty string, which is
// not `flat`: an omitted `foldPosture` keeps the current posture, per `viewport.set`'s
// "omitted fields keep their value").
[[nodiscard]] std::optional<FoldPosture> parse_fold_posture(std::string_view text) noexcept;

// The wire name of `posture` ("none", "flat", "book", "tabletop"), matching
// `parse_fold_posture` so a round trip is lossless. This is what the protocol and the page use.
[[nodiscard]] std::string_view fold_posture_name(FoldPosture posture) noexcept;

// The band the hinge occupies along its axis, in logical pixels. It is a fixed simulated value (a
// real device reports its own through `FoldingFeature.getBounds`), so two runs of one posture
// produce the same rectangle and therefore the same layout.
inline constexpr float kSimulatedHingeSize = 24.0f;

// The `FoldInfo` for `posture` in a `width` x `height` window, with the hinge centred on the axis
// the posture names. Deterministic: a pure function of its arguments, never of a clock or a sensor.
//
// The band is clamped so it can never exceed the extent it splits (a 10 px window keeps a 10 px
// hinge, not a 24 px one that would make both panes negative), and `hinge_bounds` spans the full
// cross axis: that is the rectangle `resolve_safe_area` reads and the rectangle a preview overlay
// draws. `FoldPosture::None` and a zero extent yield the default "no fold" `FoldInfo`.
[[nodiscard]] FoldInfo fold_info_for(FoldPosture posture, std::uint32_t width,
                                     std::uint32_t height) noexcept;

// True when the two snapshots differ in a field `FoldChanged` reports (state, orientation,
// occlusion, separating, hinge rectangle). This is the preview's `FoldChanged`-equivalent: the
// session applies metrics only when it answers true, so an unchanged posture costs no layout pass
// — the same "no work for unchanged inputs" rule `AppHost::set_window_metrics` follows.
[[nodiscard]] bool fold_changed(const FoldInfo& before, const FoldInfo& after) noexcept;

// The rotated snapshot (docs/specs/safe-area-v1.md section 4 and docs/design/responsive.md
// section 5): a rotation of a foldable swaps the roles of the two hinge axes, so a `book` fold
// seen in the rotated frame is a horizontal hinge and a `tabletop` fold is a vertical one. The
// hinge is re-centred in the new extent, because the device turned and the window with it.
//
// A `None`, `Flat` or already-empty fold is returned unchanged: there is no axis to swap.
[[nodiscard]] FoldInfo rotate_fold(const FoldInfo& fold, std::uint32_t width,
                                   std::uint32_t height) noexcept;

} // namespace tessera::hal
