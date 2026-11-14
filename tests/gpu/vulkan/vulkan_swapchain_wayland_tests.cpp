// SPDX-License-Identifier: MIT
//
// Wayland presentation smoke test (issue #155): open a real xdg-shell window, create a Vulkan
// device that presents through VK_KHR_wayland_surface, build a swapchain on the window's
// wl_surface, render one frame into an acquired image and present it, then tear everything down.
//
// Skips (exit 0, "skipped" line) when there is no Wayland compositor, no Vulkan device, or the
// driver does not expose the Wayland surface extension. Runs headless under a weston headless
// compositor with lavapipe, which is how ci/gpu_tests and docs/guides/real-gpu-ci.md drive it.

#include "swapchain_render.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "vulkan_swapchain.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>
#include <utility>
namespace {

using tessera::rendering::rhi::Swapchain;
using tessera::rendering::rhi::SwapchainAlphaMode;
using tessera::rendering::rhi::SwapchainDesc;
using tessera::rendering::rhi::SwapchainError;
using tessera::rendering::vulkan::VulkanSwapchain;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "vulkan_swapchain_wayland: " << message << "\n";
    }
    return condition;
}

// Presents may report Suboptimal after a successful present; treat that as success in the smoke
// test because the image reached the presentation engine.
bool present_ok(Swapchain& swapchain, std::uint32_t image) {
    const auto presented = swapchain.present(image);
    if (presented) {
        return true;
    }
    return presented.error() == SwapchainError::Suboptimal;
}

} // namespace

int main() {
    const char* wayland_display = std::getenv("WAYLAND_DISPLAY");
    if (wayland_display == nullptr || wayland_display[0] == '\0') {
        std::cout << "vulkan_swapchain_wayland: skipped (no Wayland compositor: WAYLAND_DISPLAY is "
                     "not set)\n";
        return 0;
    }
    setenv("TESSERA_WINDOW_BACKEND", "wayland", 1);

    const std::string title = "Tessera Vulkan Wayland swapchain test " +
                              std::to_string(static_cast<unsigned long>(getpid()));

    tessera::hal::WindowDesc window_desc;
    window_desc.title = title;
    window_desc.width = 320;
    window_desc.height = 200;
    window_desc.visible = false;

    auto window = tessera::hal::create_window(window_desc);
    if (window == nullptr) {
        std::cout << "vulkan_swapchain_wayland: skipped (no Wayland compositor: window creation "
                     "failed)\n";
        return 0;
    }
    const tessera::hal::NativeWindowHandle native = window->native_handle();
    if (!expect(native.display != nullptr && native.window != nullptr,
                "the Wayland window publishes wl_display + wl_surface")) {
        return 1;
    }

    tessera::rendering::rhi::DeviceDesc device_desc;
    device_desc.backend = tessera::rendering::rhi::Backend::Vulkan;
    device_desc.validation = true;
    // This test presents to the Wayland window it just created: pin the surface family so a Wayland
    // session with DISPLAY set (XWayland) cannot resolve to Xlib and reject the wl_surface.
    device_desc.surface = tessera::rendering::rhi::SurfacePlatform::Wayland;

    std::unique_ptr<tessera::rendering::rhi::Device> device;
    try {
        device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "vulkan_swapchain_wayland: skipped (Vulkan device unavailable: "
                  << error.what() << ")\n";
        return 0;
    }

    const tessera::hal::WindowMetrics initial = window->metrics();
    SwapchainDesc desc;
    desc.extent = {initial.physical_width, initial.physical_height};
    desc.image_count = 3;
    desc.vsync = true;
    // A Wayland surface composites per-pixel alpha intrinsically, so the request should resolve to
    // premultiplied; the assertion below still checks the reported mode against the capability so a
    // driver that only offers opaque is the documented fallback rather than a failure.
    desc.per_pixel_alpha = true;
    desc.native_display = native.display;
    desc.native_window = native.window;

    auto created = device->create_swapchain(desc);
    if (!created) {
        if (created.error() == SwapchainError::NotSupported) {
            std::cout << "vulkan_swapchain_wayland: skipped (Wayland presentation surface "
                         "unavailable: the driver does not expose VK_KHR_wayland_surface)\n";
            device.reset();
            window.reset();
            return 0;
        }
        std::cerr << "vulkan_swapchain_wayland: create_swapchain failed\n";
        device.reset();
        window.reset();
        return 2;
    }
    std::unique_ptr<Swapchain> swapchain = std::move(*created);
    auto* native_swapchain = dynamic_cast<VulkanSwapchain*>(swapchain.get());
    if (!expect(native_swapchain != nullptr, "create_swapchain returns VulkanSwapchain")) {
        return 3;
    }
    if (!expect(swapchain->extent().width == initial.physical_width &&
                    swapchain->extent().height == initial.physical_height,
                "the swapchain adopts the configured window size") ||
        !expect(swapchain->image_count() >= 2, "the swapchain has at least two images")) {
        return 4;
    }
    const bool alpha_supported = swapchain->supports_per_pixel_alpha();
    const bool alpha_resolved =
        swapchain->alpha_mode() ==
        (alpha_supported ? SwapchainAlphaMode::PreMultiplied : SwapchainAlphaMode::Opaque);
    if (!expect(alpha_resolved, "the composite alpha mode matches the surface capability")) {
        return 5;
    }

    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    tessera::rendering::rhi::TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    auto white = device->create_texture(white_desc);
    // The pipeline renders into swapchain images, so it must use the surface format the driver
    // chose.
    auto pipeline_desc = tessera::rendering::vulkan::batched2d_pipeline_desc();
    pipeline_desc.color_format = swapchain->format();
    auto pipeline = device->create_pipeline(pipeline_desc);
    auto renderer = std::make_unique<tessera::rendering::gpu::Batched2DRenderer>(
        *device, *pipeline, *white, tessera::rendering::gpu::Batched2DConfig{4096, true, true});

    // The acceptance gate: acquire an image, render a frame into it and present it. The HAL window
    // must be shown first so the toplevel is mapped; presentation is what attaches the first
    // buffer to the surface.
    window->show();
    window->request_redraw();

    auto acquired = swapchain->acquire_next_image(1000);
    if (!expect(acquired.has_value(), "acquire succeeds")) {
        return 6;
    }
    const std::uint32_t presented_image = acquired.value();
    if (!expect(presented_image < swapchain->image_count(), "the acquired index is in range") ||
        !expect(
            tessera::test::render_swapchain_frame(*device, *renderer, *swapchain, presented_image),
            "render one frame to the swapchain image") ||
        !expect(present_ok(*swapchain, presented_image),
                "present the frame on the Wayland surface")) {
        return 7;
    }

    const std::uint32_t presents = native_swapchain->stats().presents;
    if (!expect(presents >= 1, "the swapchain counted the presented frame")) {
        return 8;
    }

    // Drain the submitted work before tearing down the resources it references.
    device->wait_idle();

    // Close the window through the HAL contract (close -> Close event) before destroying it, so the
    // smoke covers the whole acceptance path: open, present a frame, close.
    window->close();
    std::array<tessera::hal::WindowEvent, 32> events{};
    std::size_t event_count = 0;
    for (std::size_t i = 0; i < 100 && event_count == 0; ++i) {
        event_count = window->poll_events(events);
    }
    bool close_seen = false;
    for (std::size_t i = 0; i < event_count; ++i) {
        close_seen = close_seen || events[i].type == tessera::hal::WindowEventType::Close;
    }
    if (!expect(close_seen, "close() delivers a Close event")) {
        return 9;
    }

    renderer.reset();
    white.reset();
    pipeline.reset();
    swapchain.reset();
    device.reset();
    window.reset();

    std::cout << "vulkan_swapchain_wayland: ok (" << initial.logical_width << "x"
              << initial.logical_height << " on " << wayland_display << ", presents=" << presents
              << ", alpha=" << (alpha_supported ? "premultiplied" : "opaque") << ")\n";
    return 0;
}
