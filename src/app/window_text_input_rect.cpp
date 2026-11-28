// SPDX-License-Identifier: MIT
//
// The app half of the IME anchor seam (issue #1053): resolve the focused text field's caret box and
// publish it to the platform window only when it moved.

#include "tessera/ui/paint/text_field_geometry.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "window_text_input_rect.hpp"

namespace tessera::app::detail {
namespace {

std::size_t anchor_offset(const ui::TextDecoration& decoration) {
    return decoration.composition_active ? decoration.preedit_begin : decoration.caret_byte_offset;
}

} // namespace

std::optional<hal::TextInputRect>
focused_caret_rect(const ui::Node* focused, ui::paint::StyleProvider& styles,
                   const ui::paint::PaintTextMetricsSource* metrics, float device_scale) {
    if (focused == nullptr) {
        return std::nullopt;
    }
    const std::optional<ui::PaintData>& data = focused->paint_data();
    if (!data || data->kind != ui::PaintKind::Text || !data->decoration) {
        return std::nullopt;
    }
    const auto box =
        ui::paint::text_field_caret_box(*focused, styles.resolve(*focused), focused->world_bounds(),
                                        anchor_offset(*data->decoration), metrics, device_scale);
    if (!box) {
        return std::nullopt;
    }
    return hal::TextInputRect{box->x, box->y, box->width, box->height};
}

bool sync_text_input_rect(hal::Window& window, const ui::Node* focused,
                          ui::paint::StyleProvider& styles,
                          const ui::paint::PaintTextMetricsSource* metrics, float device_scale,
                          TextInputRectState& state) {
    if (focused == nullptr) {
        state.valid = false;
        return false;
    }
    const std::optional<ui::PaintData>& data = focused->paint_data();
    if (!data || data->kind != ui::PaintKind::Text || !data->decoration) {
        state.valid = false;
        return false;
    }
    const auto bounds = focused->world_bounds();
    TextInputRectState key;
    key.node = focused;
    key.caret_byte_offset = anchor_offset(*data->decoration);
    key.text_size = data->text.size();
    key.x = bounds.x;
    key.y = bounds.y;
    key.width = bounds.width;
    key.height = bounds.height;
    key.device_scale = device_scale;
    key.valid = true;
    if (state.valid && state.node == key.node && state.caret_byte_offset == key.caret_byte_offset &&
        state.text_size == key.text_size && state.x == key.x && state.y == key.y &&
        state.width == key.width && state.height == key.height &&
        state.device_scale == key.device_scale) {
        return false;
    }
    key.sent = state.sent;
    state = key;
    const std::optional<hal::TextInputRect> rect =
        focused_caret_rect(focused, styles, metrics, device_scale);
    if (!rect) {
        return false;
    }
    if (state.sent && state.sent->x == rect->x && state.sent->y == rect->y &&
        state.sent->width == rect->width && state.sent->height == rect->height) {
        return false;
    }
    window.set_text_input_rect(*rect);
    state.sent = *rect;
    return true;
}

} // namespace tessera::app::detail
