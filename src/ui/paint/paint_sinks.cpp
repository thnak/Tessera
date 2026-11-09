// SPDX-License-Identifier: MIT

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"

namespace tessera::ui::paint {

Batched2DSink::Batched2DSink(rendering::gpu::Batched2DRenderer& renderer)
    : renderer_(&renderer) {}

void Batched2DSink::fill_rect(const rhi::Rect& rect, const rhi::Color& color) {
    renderer_->push_rect(rect, color);
}

void Batched2DSink::fill_rounded_rect(const rhi::Rect& rect, const rhi::Color& color,
                                      float radius) {
    renderer_->push_rounded_rect(rect, color, radius);
}

} // namespace tessera::ui::paint
