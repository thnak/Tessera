// SPDX-License-Identifier: MIT

#include "tessera/app/window.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "window_internal.hpp"

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace tessera::app {
namespace {

namespace rhi = tessera::rendering::rhi;

rhi::Extent2D physical_extent(const CaptureOptions& options) {
    if (options.width == 0 || options.height == 0) {
        throw std::invalid_argument("capture_frame: width and height must be positive");
    }
    if (!std::isfinite(options.dpi_scale) || options.dpi_scale <= 0.0f) {
        throw std::invalid_argument("capture_frame: dpi scale must be finite and positive");
    }
    const auto scaled = [](std::uint32_t logical, float dpi) {
        return static_cast<std::uint32_t>(std::ceil(static_cast<float>(logical) * dpi));
    };
    return rhi::Extent2D{scaled(options.width, options.dpi_scale),
                         scaled(options.height, options.dpi_scale)};
}

} // namespace

std::optional<CapturedFrame> capture_frame(ui::Node& root, ui::style::CompiledStyleSheet sheet,
                                           const CaptureOptions& options,
                                           const std::function<void()>& prepare) {
    const rhi::Extent2D extent = physical_extent(options);
    auto device = detail::create_device();
    if (device == nullptr) {
        return std::nullopt;
    }

    const rhi::TextureFormat format = rhi::TextureFormat::R8G8B8A8Unorm;
    auto pipeline = detail::create_pipeline(*device, format);
    auto white = detail::create_white(*device);
    rendering::gpu::Batched2DRenderer renderer(*device, *pipeline, *white,
                                               rendering::gpu::Batched2DConfig{4096, true, false});
    detail::TextStack text(*device, options.font_path);
    ui::paint::Batched2DSink sink(renderer, text.renderer.get(), text.shaper.get());

    runtime::ManualClock clock;
    runtime::RuntimeLoop loop(clock);
    HostConfig config;
    config.width = static_cast<float>(options.width);
    config.height = static_cast<float>(options.height);
    config.dpi_scale = options.dpi_scale;
    config.text_measurer = options.text_measurer;
    AppHost host(root, std::move(sheet), loop, config);
    if (options.scheduler != nullptr) {
        host.set_update_scheduler(options.scheduler);
    }
    // First frame lays the scene out so `prepare` can hit-test or change state; the second frame
    // applies that state (scheduler flush, style, layout) before the captured paint.
    (void)host.begin_frame(0.0);
    if (prepare) {
        prepare();
    }
    (void)host.begin_frame(0.0);

    const rhi::RenderTargetDesc desc = detail::target_desc(extent, format);
    auto target = device->create_render_target(desc);
    auto commands = device->create_command_list();
    renderer.begin(*commands, *target, desc);
    sink.begin_text_frame();
    ui::paint::paint_scene(root, host.styles(), sink);
    renderer.end();
    commands->submit();

    const std::size_t bytes =
        static_cast<std::size_t>(extent.width) * static_cast<std::size_t>(extent.height) * 4;
    rhi::BufferDesc readback_desc;
    readback_desc.size = bytes;
    readback_desc.usage = rhi::BufferUsage::Readback;
    auto readback = device->create_buffer(readback_desc);
    auto copy = device->create_command_list();
    copy->copy_target_to_buffer(*target, *readback);
    copy->submit();

    CapturedFrame frame;
    frame.width = extent.width;
    frame.height = extent.height;
    frame.rgba.resize(bytes);
    readback->read(frame.rgba.data(), frame.rgba.size());
    return frame;
}

} // namespace tessera::app
