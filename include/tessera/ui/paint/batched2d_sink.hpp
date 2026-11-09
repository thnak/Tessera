// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/paint/paint_sink.hpp"

namespace tessera::rendering::gpu {
class Batched2DRenderer;
} // namespace tessera::rendering::gpu

namespace tessera::ui::paint {

// Forwards paint commands to the batched 2D renderer. Deliberately not a backend adapter: the
// renderer already owns the only GPU seam (rhi::CommandList), so this header stays free of
// Vulkan/WebGPU includes.
class Batched2DSink final : public PaintSink {
public:
    explicit Batched2DSink(rendering::gpu::Batched2DRenderer& renderer);

    void fill_rect(const rhi::Rect& rect, const rhi::Color& color) override;
    void fill_rounded_rect(const rhi::Rect& rect, const rhi::Color& color, float radius) override;
    void push_clip(const RoundedClip& clip) override;
    void pop_clip() override;

private:
    rendering::gpu::Batched2DRenderer* renderer_ = nullptr;
};

} // namespace tessera::ui::paint
