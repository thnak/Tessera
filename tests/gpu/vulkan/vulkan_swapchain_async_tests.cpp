// SPDX-License-Identifier: MIT
//
// Frame loop with asynchronous submission and two frames in flight: each frame is queued with
// submit_async() and presented immediately, without waiting for the GPU, and the loop only waits
// for the frame two iterations back (the one whose vertex buffer slot is about to be reused).
// Present must wait for rendering through the swapchain image's semaphore. Synchronization
// validation reports a present that races the render as "[vulkan] ... hazard", which fails this
// test in ctest.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "swapchain_render.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <windows.h>

namespace {

using tessera::rendering::rhi::SwapchainDesc;
using tessera::rendering::rhi::SwapchainError;

constexpr int kFrames = 12;
constexpr std::size_t kFramesInFlight = 2;
constexpr std::uint64_t kAcquireTimeoutMs = 1000;
constexpr std::uint64_t kFrameWaitMs = 5000;
constexpr int kMaxRetries = 20;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "vulkan_swapchain_async: " << message << "\n";
    }
    return condition;
}

HWND find_window(const std::string& title) {
    return FindWindowA(nullptr, title.c_str());
}

} // namespace

int main() {
    const std::string title =
        "Tessera async swapchain test " + std::to_string(GetCurrentProcessId());

    tessera::hal::WindowDesc window_desc;
    window_desc.title = title;
    window_desc.width = 320;
    window_desc.height = 200;
    window_desc.visible = false;

    auto window = tessera::hal::create_window(window_desc);
    if (!expect(window != nullptr, "create_window returns a window")) {
        return 1;
    }
    const HWND hwnd = find_window(title);
    if (!expect(hwnd != nullptr, "native window exists")) {
        return 2;
    }

    tessera::rendering::rhi::DeviceDesc device_desc;
    device_desc.backend = tessera::rendering::rhi::Backend::Vulkan;
    device_desc.validation = true;

    std::unique_ptr<tessera::rendering::rhi::Device> device;
    try {
        device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "vulkan_swapchain_async: skipped (Vulkan device unavailable: " << error.what()
                  << ")\n";
        return 0;
    }

    const tessera::hal::WindowMetrics metrics = window->metrics();
    SwapchainDesc desc;
    desc.extent = {metrics.physical_width, metrics.physical_height};
    desc.image_count = 3;
    desc.vsync = true;
    desc.native_window = hwnd;

    auto created = device->create_swapchain(desc);
    if (!created) {
        if (created.error() == SwapchainError::NotSupported) {
            std::cout << "vulkan_swapchain_async: skipped (presentation surface unavailable)\n";
            return 0;
        }
        std::cerr << "vulkan_swapchain_async: create_swapchain failed\n";
        return 3;
    }
    std::unique_ptr<tessera::rendering::rhi::Swapchain> swapchain = std::move(*created);

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
    tessera::rendering::gpu::Batched2DConfig renderer_config;
    renderer_config.golden_mode = true;
    renderer_config.frames_in_flight = kFramesInFlight;
    auto renderer = std::make_unique<tessera::rendering::gpu::Batched2DRenderer>(
        *device, *pipeline, *white, renderer_config);

    // The list of the frame that last used each vertex-buffer slot.
    std::array<std::unique_ptr<tessera::rendering::rhi::CommandList>, kFramesInFlight> in_flight;

    int acquire_timeouts = 0;
    for (int frame = 0; frame < kFrames; ++frame) {
        auto& slot = in_flight[static_cast<std::size_t>(frame) % kFramesInFlight];

        // Beginning this frame reuses the slot's vertex buffer: its previous frame must be done.
        if (slot != nullptr) {
            if (!expect(slot->wait(kFrameWaitMs), "frame two iterations back completes")) {
                return 7;
            }
            slot.reset();
        }

        // Acquire waits a bounded time (ADR-021), so a slow presentation engine legitimately
        // answers Timeout; the real loop retries or skips the frame, and so does this one. The
        // first frame that has to wait for an image to come back (frame 3 with three images) took
        // more than the bound on an AMD iGPU, so the retry count is reported.
        auto acquired = swapchain->acquire_next_image(kAcquireTimeoutMs);
        for (int attempt = 0; !acquired.has_value() &&
                              acquired.error() == SwapchainError::Timeout && attempt < kMaxRetries;
             ++attempt) {
            ++acquire_timeouts;
            acquired = swapchain->acquire_next_image(kAcquireTimeoutMs);
        }
        if (!acquired.has_value()) {
            std::cerr << "vulkan_swapchain_async: acquire failed at frame " << frame
                      << " with error " << static_cast<int>(acquired.error()) << '\n';
            return 4;
        }
        if (!expect(tessera::test::render_swapchain_frame(*device, *renderer, *swapchain,
                                                          acquired.value(), true, &slot),
                    "render to swapchain image")) {
            return 5;
        }

        // Present right behind the async submit, before the GPU is known to be done.
        const auto presented = swapchain->present(acquired.value());
        if (!expect(presented.has_value() || presented.error() == SwapchainError::Suboptimal,
                    "present succeeds")) {
            return 6;
        }
    }

    // Drain before tearing down the resources the last frames still reference.
    device->wait_idle();
    in_flight = {};
    renderer.reset();
    white.reset();
    pipeline.reset();
    swapchain.reset();
    device.reset();
    window.reset();

    std::cout << "vulkan_swapchain_async: ok (" << kFrames << " frames, " << acquire_timeouts
              << " acquire timeouts)\n";
    return 0;
}
