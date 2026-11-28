// SPDX-License-Identifier: MIT
//
// App-side IME anchor plumbing (issue #1053, follow-up of #55): the focused text field's caret
// rectangle, in logical window pixels, handed to `hal::Window::set_text_input_rect` so the platform
// can place the candidate window (X11: XNSpotLocation).
//
// The geometry is the paint pass's own (`ui::paint::text_field_caret_box`), so the IME anchor and
// the drawn caret cannot drift apart. Not a public header.

#pragma once

#include "tessera/hal/window.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/paint/paint_text_metrics.hpp"
#include "tessera/ui/scene/node_fwd.hpp"

#include <cstddef>
#include <optional>

namespace tessera::app::detail {

// What the last publish was computed from. The caret box needs a text layout, so the driver
// recomputes it only when one of these inputs changes: a static focused field costs a comparison
// per iteration, and an unfocused window costs nothing.
struct TextInputRectState {
    const ui::Node* node = nullptr;
    std::size_t caret_byte_offset = 0;
    std::size_t preedit_begin = 0;
    std::size_t text_size = 0;
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
    float device_scale = 1.0F;
    bool valid = false;
    // The last rect handed to the window, so an equal result makes no platform call.
    std::optional<hal::TextInputRect> sent;
};

// The caret rect of `focused` in window logical pixels (the node's world bounds plus the caret
// box), or nullopt when `focused` is null, is not a text field carrying a caret decoration, or has
// no room for text. While an IME composition is active the anchor is the preedit start, so the
// candidate window sits at the composed text rather than after it.
[[nodiscard]] std::optional<hal::TextInputRect>
focused_caret_rect(const ui::Node* focused, ui::paint::StyleProvider& styles,
                   const ui::paint::PaintTextMetricsSource* metrics, float device_scale);

// Publishes the focused field's caret rect to `window` when it changed since `state`. Losing focus
// publishes nothing (the platform keeps the last spot; a field that regains focus republishes).
// Returns true when a platform call was made. Never blocks and never throws.
bool sync_text_input_rect(hal::Window& window, const ui::Node* focused,
                          ui::paint::StyleProvider& styles,
                          const ui::paint::PaintTextMetricsSource* metrics, float device_scale,
                          TextInputRectState& state);

} // namespace tessera::app::detail
