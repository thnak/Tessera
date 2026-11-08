// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cstddef>

namespace tessera::ui::layout {

struct LayoutApplyResult {
    std::size_t applied = 0;
    std::size_t missing = 0;
    std::size_t extra = 0;
};

LayoutApplyResult apply_layout(const LayoutResult& result, Node& root);

} // namespace tessera::ui::layout
