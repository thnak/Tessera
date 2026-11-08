// SPDX-License-Identifier: MIT

#include "swapchain_render.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "vulkan_internal.hpp"

#include <X11/Xlib.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace {

using tessera::rendering::rhi::Swapchain;
using tessera::rendering::rhi::SwapchainDesc;
using tessera::rendering::rhi::SwapchainError;
using tessera::rendering::vulkan::VulkanSwapchain;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "vulkan_swapchain_x11: " << message << "\n";
    }
    return condition;
}

// The HAL keeps its native handles private, so the test locates the window as an external client
// would and supplies Display* + Window through SwapchainDesc.
::Window find_window(Display* display, ::Window window, std::string_view title) {
    char* name = nullptr;
    if (XFetchName(display, window, &name) != 0 && name != nullptr) {
        const bool matches = title == name;
        XFree(name);
        if (matches) {
            return window;
        }
    }

    ::Window root = 0;
    ::Window parent = 0;
    ::Window* children = nullptr;
    unsigned int child_count = 0;
    if (XQueryTree(display, window, &root, &parent, &children, &child_count) == 0) {
        return 0;
    }
    ::Window found = 0;
    for (unsigned int i = 0; i < child_count && found == 0; ++i) {
        found = find_window(display, children[i], title);
    }
    if (children != nullptr) {
        XFree(children);
    }
    return found;
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
    const char* display_name = std::getenv("DISPLAY");
    if (display_name == nullptr || display_name[0] == '\0') {
        std::cout << "vulkan_swapchain_x11: skipped (DISPLAY is not set)\n";
        return 0;
    }
    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        std::cout << "vulkan_swapchain_x11: skipped (cannot open DISPLAY)\n";
        return 0;
    }

    const std::string title =
        "Tessera Vulkan X11 swapchain test " + std::to_string(static_cast<unsigned long>(getpid()));

    tessera::hal::WindowDesc window_desc;
    window_desc.title = title;
    window_desc.width = 320;
    window_desc.height = 200;
    window_desc.visible = false;

    auto window = tessera::hal::create_window(window_desc);
    if (!expect(window != nullptr, "create_window returns a window")) {
        XCloseDisplay(display);
        return 1;
    }
    const ::Window native = find_window(display, DefaultRootWindow(display), title);
    if (!expect(native != 0, "native window exists")) {
        XCloseDisplay(display);
        return 2;
    }

    tessera::rendering::rhi::DeviceDesc device_desc;
    device_desc.backend = tessera::rendering::rhi::Backend::Vulkan;
    device_desc.validation = false;

    std::unique_ptr<tessera::rendering::rhi::Device> device;
    try {
        device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "vulkan_swapchain_x11: skipped (Vulkan device unavailable: " << error.what()
                  << ")\n";
        XCloseDisplay(display);
        return 0;
    }

    const tessera::hal::WindowMetrics initial = window->metrics();
    SwapchainDesc desc;
    desc.extent = {initial.physical_width, initial.physical_height};
    desc.image_count = 3;
    desc.vsync = true;
    desc.native_display = display;
    desc.native_window = reinterpret_cast<void*>(static_cast<std::uintptr_t>(native));

    auto created = device->create_swapchain(desc);
    if (!created) {
        if (created.error() == SwapchainError::NotSupported) {
            std::cout << "vulkan_swapchain_x11: skipped (Xlib presentation surface unavailable)\n";
            device.reset();
            window.reset();
            XCloseDisplay(display);
            return 0;
        }
        std::cerr << "vulkan_swapchain_x11: create_swapchain failed\n";
        device.reset();
        window.reset();
        XCloseDisplay(display);
        return 3;
    }
    std::unique_ptr<Swapchain> swapchain = std::move(*created);
    auto* native_swapchain = dynamic_cast<VulkanSwapchain*>(swapchain.get());
    if (!expect(native_swapchain != nullptr, "create_swapchain returns VulkanSwapchain")) {
        XCloseDisplay(display);
        return 4;
    }
    if (!expect(swapchain->extent().width == initial.physical_width &&
                    swapchain->extent().height == initial.physical_height,
                "swapchain adopts the window size") ||
        !expect(swapchain->image_count() >= 2, "swapchain has at least two images")) {
        XCloseDisplay(display);
        return 5;
    }

    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    tessera::rendering::rhi::TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    auto white = device->create_texture(white_desc);
    auto pipeline = device->create_pipeline(tessera::rendering::vulkan::batched2d_pipeline_desc());
    auto renderer = std::make_unique<tessera::rendering::gpu::Batched2DRenderer>(
        *device, *pipeline, *white, tessera::rendering::gpu::Batched2DConfig{4096, true, true});

    auto acquired = swapchain->acquire_next_image(1000);
    if (!expect(acquired.has_value(), "acquire succeeds")) {
        XCloseDisplay(display);
        return 6;
    }
    if (!expect(acquired.value() < swapchain->image_count(), "acquired index is in range") ||
        !expect(
            tessera::test::render_swapchain_frame(*device, *renderer, *swapchain, acquired.value()),
            "render to swapchain image") ||
        !expect(present_ok(*swapchain, acquired.value()), "present succeeds")) {
        XCloseDisplay(display);
        return 7;
    }

    // Resize the native window, drain the HAL's ConfigureNotify, then resize the swapchain. The
    // first acquire retires the old swapchain and reports OutOfDate while adopting the new extent.
    XResizeWindow(display, native, 400, 300);
    XSync(display, False);
    if (!expect(window->wait_for_events(1.0), "resize reaches the HAL")) {
        XCloseDisplay(display);
        return 8;
    }
    const tessera::hal::WindowMetrics resized = window->metrics();
    if (!expect(resized.physical_width == 400 && resized.physical_height == 300, "resized size")) {
        XCloseDisplay(display);
        return 9;
    }

    swapchain->resize({resized.physical_width, resized.physical_height});
    const auto stale = swapchain->acquire_next_image(1000);
    if (!expect(!stale.has_value() && stale.error() == SwapchainError::OutOfDate,
                "first acquire after resize is OutOfDate") ||
        !expect(swapchain->extent().width == resized.physical_width &&
                    swapchain->extent().height == resized.physical_height,
                "out-of-date acquire adopts the new extent") ||
        !expect(native_swapchain->stats().swapchains_created >= 2,
                "resize creates a replacement swapchain") ||
        !expect(native_swapchain->stats().swapchain_retires >= 1,
                "resize retires the old swapchain")) {
        XCloseDisplay(display);
        return 10;
    }

    auto adopted = swapchain->acquire_next_image(1000);
    if (!expect(adopted.has_value(), "acquire adopts the new swapchain") ||
        !expect(
            tessera::test::render_swapchain_frame(*device, *renderer, *swapchain, adopted.value()),
            "render after resize") ||
        !expect(present_ok(*swapchain, adopted.value()), "present after resize succeeds")) {
        XCloseDisplay(display);
        return 11;
    }

    const std::uint32_t swapchains_created = native_swapchain->stats().swapchains_created;
    const std::uint32_t presents = native_swapchain->stats().presents;

    // Dependency order: the swapchain owns the surface on `display`, so it goes first; then the
    // device, the HAL window (its own connection), and finally the test's display.
    renderer.reset();
    white.reset();
    pipeline.reset();
    swapchain.reset();
    device.reset();
    window.reset();
    XCloseDisplay(display);

    std::cout << "vulkan_swapchain_x11: ok (" << swapchains_created << " swapchains, " << presents
              << " presents)\n";
    return 0;
}
