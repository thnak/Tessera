// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/scene/paint_data.hpp"

#include <string_view>

namespace tessera::ui {

// Whether a text payload wraps at its content box. A `Text` element wraps by default, as a
// browser's text does under `white-space: normal` (ADR-063); `PaintData::wrap` can still opt in any
// other carrier, and a computed `white-space: nowrap|pre` suppresses it at the use site. Layout and
// paint both ask this, so the line count layout measured is the one paint draws.
[[nodiscard]] inline bool paint_wraps(std::string_view element_kind,
                                      const PaintData& data) noexcept {
    return data.wrap == PaintWrap::Wrap || element_kind == "Text";
}

} // namespace tessera::ui
