// SPDX-License-Identifier: MIT

#pragma once

// Measured LiveCapsule slot sizes (docs/specs/live-capsule-v1.md section 3, issue #997). The
// component asks a `SlotMeasurer` for the natural extent of each built slot instead of trusting the
// size hints on `Slots`. The stock measurer is an intrinsic walk over the slot's scene nodes with a
// `text::TextMeasurer`; a host that already runs layout may install its own.

#include "tessera/ui/text/text_measure.hpp"

#include <cstdint>
#include <functional>
#include <memory>

namespace tessera::ui {
class Node;
}

namespace tessera::ui::capsule {

struct SlotExtent {
    float width = 0.0f;
    float height = 0.0f;
};

// Which text style the slot's bare text uses: the capsule label (11px) or the card body (14px),
// the two sizes live-capsule.css sets.
enum class SlotTextRole : std::uint8_t { Label, Body };

using SlotMeasurer = std::function<SlotExtent(const Node& slot, SlotTextRole role)>;

// Edge padding the CSS gives the compact slots, the card, and an action button (--space-3 and
// --control-padding-x-compact), plus the action's height and its right margin. They mirror
// packages/tessera-ui/live-capsule.css; the golden tests pin the pair together.
inline constexpr float kSlotEdgePadding = 12.0f;
inline constexpr float kCardPadding = 12.0f;
inline constexpr float kActionPaddingX = 8.0f;
inline constexpr float kActionHeight = 24.0f;
inline constexpr float kActionGap = 8.0f;
// The capsule's own 1 px border (--border-width) on each side, inside its border-box.
inline constexpr float kCapsuleBorder = 1.0f;

// Natural extent of a subtree: a Text node is measured on one line, a node with an inline px
// `width`/`height` takes it, and any other node is a row of its children (widths sum, heights
// max). Padding and margins on builder content are not counted.
[[nodiscard]] SlotExtent intrinsic_slot_extent(const Node& node, const text::TextMeasurer& measurer,
                                               SlotTextRole role);

// The stock measurer over `measurer`, which the returned function shares.
[[nodiscard]] SlotMeasurer
make_intrinsic_slot_measurer(std::shared_ptr<const text::TextMeasurer> measurer);

} // namespace tessera::ui::capsule
