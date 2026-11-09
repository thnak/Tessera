// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/path/path_model.hpp"

#include <vector>

namespace tessera::poc::thorvg_spike {

// Builds the representative icon/chart path set used by the spike and its golden: rounded-rect
// fill plus miter stroke, quadratic/cubic curves, both fill rules, open strokes with all cap and
// join styles, and a translucent overlay. Geometry is authored in a 512x512 design space and
// scaled to the requested size, so a given size always produces the same scene.
[[nodiscard]] std::vector<tessera::rendering::path::PathShape> build_icon_chart_scene(float width,
                                                                                      float height);

} // namespace tessera::poc::thorvg_spike
