// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"

namespace tessera::ui::layout {

float round_half_up(float value);
float snap_scalar(float value, float dpi_scale);
tessera::rendering::rhi::Rect snap_rect(const tessera::rendering::rhi::Rect& bounds,
                                        float dpi_scale);

} // namespace tessera::ui::layout
