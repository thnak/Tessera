// SPDX-License-Identifier: MIT
//
// The frame transform's clip seam (docs/design/display-aware-rendering.md section 6). A clip is
// authored in paint (CSS) coordinates; Batched2DRenderer::push_clip scales it by the frame's
// content scale and derives the CPU scissor from that scaled rect. This proves the derived scissor
// really lands in target pixels, so a hi-DPI frame is not cut at the clip's CSS position. The null
// device records the scissor each draw ran with, so no GPU is needed.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/gpu/clip.hpp"
#include "tessera/rendering/null/null_device.hpp"
#include "tessera/ui/scale/render_target_extent.hpp"

#include <cstdint>
#include <iostream>

namespace {

using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::null::NullCommandList;
using tessera::rendering::rhi::RenderTargetDesc;
using tessera::rendering::rhi::rgba8;
using tessera::rendering::rhi::Scissor;
using tessera::ui::scale::content_scale_for;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "content_scale: " << message << "\n";
    }
    return condition;
}

// Paints one clipped quad and returns the scissor its draw ran with. `device_scale` is the value
// the window composes into the frame's content scale (1.0 = the pre-transform path).
Scissor clipped_quad_scissor(tessera::rendering::rhi::Device& device, Batched2DRenderer& renderer,
                             float device_scale) {
    auto target = device.create_render_target({{64, 64}});
    auto commands = device.create_command_list();
    renderer.begin(*commands, *target, RenderTargetDesc{{64, 64}});
    const auto transform = content_scale_for(device_scale);
    if (!transform.is_identity()) {
        renderer.set_content_scale(transform.x, transform.y);
    }
    // A CSS-space clip and a quad inside it, exactly as the paint pass pushes them.
    renderer.push_clip(
        tessera::rendering::gpu::clip::uniform_rect({4.0f, 4.0f, 10.0f, 10.0f}, 0.0f));
    renderer.push_rect({4.0f, 4.0f, 10.0f, 10.0f}, rgba8(255, 255, 255));
    renderer.pop_clip();
    renderer.end();
    commands->submit();

    auto* null_commands = dynamic_cast<NullCommandList*>(commands.get());
    if (null_commands == nullptr || null_commands->stats().draw_records.size() != 1) {
        return Scissor{};
    }
    return null_commands->stats().draw_records.front().scissor;
}

} // namespace

int main() {
    auto device = tessera::rendering::null::create_null_device({});
    auto white = device->create_texture({{1, 1}});
    auto pipeline = device->create_pipeline({});
    Batched2DRenderer renderer(*device, *pipeline, *white);

    bool ok = true;

    // The pre-transform path: the scissor is the clip's own CSS-space rectangle.
    const Scissor plain = clipped_quad_scissor(*device, renderer, 1.0f);
    ok = expect(plain.x == 4 && plain.y == 4 && plain.width == 10 && plain.height == 10,
                "device scale 1 keeps the clip scissor in paint space") &&
         ok;

    // Device scale 2: the clip, its folded SDF uniform and the CPU scissor all scale by the frame's
    // content scale, so the visible cut lands where the scaled geometry lands.
    const Scissor scaled = clipped_quad_scissor(*device, renderer, 2.0f);
    ok = expect(scaled.x == 8 && scaled.y == 8 && scaled.width == 20 && scaled.height == 20,
                "device scale 2 scales the clip scissor into target pixels") &&
         ok;

    if (!ok) {
        std::cerr << "content_scale: FAILED\n";
        return 1;
    }
    std::cout << "content_scale: ok\n";
    return 0;
}
