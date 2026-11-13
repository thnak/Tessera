// SPDX-License-Identifier: MIT

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
#include "vulkan_swapchain.hpp"

#include <array>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <windows.h>

namespace {

using tessera::rendering::rhi::Swapchain;
using tessera::rendering::rhi::SwapchainAlphaMode;
using tessera::rendering::rhi::SwapchainDesc;
using tessera::rendering::rhi::SwapchainError;
using tessera::rendering::rhi::TextureFormat;
using tessera::rendering::vulkan::VulkanSwapchain;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "vulkan_swapchain: " << message << "\n";
    }
    return condition;
}

const char* format_name(TextureFormat format) {
    switch (format) {
    case TextureFormat::R8G8B8A8Unorm:
        return "R8G8B8A8Unorm";
    case TextureFormat::R8Unorm:
        return "R8Unorm";
    case TextureFormat::B8G8R8A8Unorm:
        return "B8G8R8A8Unorm";
    case TextureFormat::R8G8B8A8Srgb:
        return "R8G8B8A8Srgb";
    case TextureFormat::B8G8R8A8Srgb:
        return "B8G8R8A8Srgb";
    }
    return "unknown";
}

void pump_messages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

HWND find_window(std::string_view title) {
    return FindWindowA(nullptr, std::string(title).c_str());
}

bool resize_client(HWND window, std::uint32_t client_width, std::uint32_t client_height) {
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
    const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    RECT rect{0, 0, static_cast<LONG>(client_width), static_cast<LONG>(client_height)};
    if (AdjustWindowRectExForDpi(&rect, style, FALSE, ex_style, GetDpiForWindow(window)) == FALSE) {
        return false;
    }
    return SetWindowPos(window, nullptr, 0, 0, static_cast<int>(rect.right - rect.left),
                        static_cast<int>(rect.bottom - rect.top),
                        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER) != FALSE;
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
    const std::string title =
        "Tessera Vulkan swapchain test " + std::to_string(GetCurrentProcessId());

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
        std::cout << "vulkan_swapchain: skipped (Vulkan device unavailable: " << error.what()
                  << ")\n";
        return 0;
    }

    const tessera::hal::WindowMetrics initial = window->metrics();
    SwapchainDesc desc;
    desc.extent = {initial.physical_width, initial.physical_height};
    desc.image_count = 3;
    desc.vsync = true;
    // A translucent window requests per-pixel alpha (glass-v1.md section 6); the surface decides
    // whether it can honor the request, and the resolved mode is reported back.
    desc.per_pixel_alpha = true;
    desc.native_window = hwnd;

    auto created = device->create_swapchain(desc);
    if (!created) {
        if (created.error() == SwapchainError::NotSupported) {
            std::cout << "vulkan_swapchain: skipped (presentation surface unavailable)\n";
            return 0;
        }
        std::cerr << "vulkan_swapchain: create_swapchain failed\n";
        return 3;
    }
    std::unique_ptr<Swapchain> swapchain = std::move(*created);
    auto* native_swapchain = dynamic_cast<VulkanSwapchain*>(swapchain.get());
    if (!expect(native_swapchain != nullptr, "create_swapchain returns VulkanSwapchain")) {
        return 4;
    }
    if (!expect(swapchain->extent().width == initial.physical_width &&
                    swapchain->extent().height == initial.physical_height,
                "swapchain adopts the window client size") ||
        !expect(swapchain->image_count() >= 2, "swapchain has at least two images")) {
        return 5;
    }

    // D2: the surface picks an sRGB format when the platform offers one, otherwise it keeps the
    // historical UNORM selection. The preference order itself is unit-tested with plain data
    // (vulkan_surface_format); this checks the real driver result is one of the supported formats.
    const TextureFormat chosen_format = swapchain->format();
    if (!expect(chosen_format == TextureFormat::R8G8B8A8Unorm ||
                    chosen_format == TextureFormat::B8G8R8A8Unorm ||
                    chosen_format == TextureFormat::R8G8B8A8Srgb ||
                    chosen_format == TextureFormat::B8G8R8A8Srgb,
                "swapchain chooses a supported surface format")) {
        return 5;
    }
    const bool srgb_swapchain = tessera::rendering::rhi::is_srgb(chosen_format);

    // The composite-alpha contract: a premultiplied mode exactly when the surface offers one, and
    // the reported mode matches the capability. The test skips cleanly on a surface without alpha
    // (a 24-bit visual, a headless WSI); the fallback is verified as Opaque, not as a failure.
    const bool alpha_supported = swapchain->supports_per_pixel_alpha();
    if (!expect(swapchain->alpha_mode() == (alpha_supported ? SwapchainAlphaMode::PreMultiplied
                                                            : SwapchainAlphaMode::Opaque),
                "composite alpha mode matches the surface capability")) {
        return 19;
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

    auto acquired = swapchain->acquire_next_image(1000);
    if (!expect(acquired.has_value(), "acquire succeeds")) {
        return 6;
    }
    if (!expect(acquired.value() < swapchain->image_count(), "acquired index is in range") ||
        !expect(
            tessera::test::render_swapchain_frame(*device, *renderer, *swapchain, acquired.value()),
            "render to swapchain image")) {
        return 7;
    }
    if (!expect(present_ok(*swapchain, acquired.value()), "present succeeds")) {
        return 8;
    }

#if defined(TESSERA_VULKAN_TEST_HOOKS)
    // D7: a lost acquire invalidates the swapchain and every RHI resource; after the one-shot
    // device recovery the presenter recreates them and presents frames again.
    auto* vulkan_device = dynamic_cast<tessera::rendering::vulkan::VulkanDevice*>(device.get());
    if (!expect(vulkan_device != nullptr, "Vulkan device exposes the recovery seam")) {
        return 14;
    }
    vulkan_device->inject_fault_for_testing(
        tessera::rendering::vulkan::InjectedDeviceFault::DeviceLostOnAcquire);
    const auto lost_acquire = swapchain->acquire_next_image(100);
    if (!expect(!lost_acquire && lost_acquire.error() == SwapchainError::DeviceLost,
                "lost acquire reports DeviceLost") ||
        !expect(device->status() == tessera::rendering::rhi::DeviceStatus::Lost,
                "device records the loss")) {
        return 15;
    }

    // The caller releases the lost generation before recovering (the backend refuses otherwise).
    renderer.reset();
    white.reset();
    pipeline.reset();
    swapchain.reset();
    if (!expect(device->recover().has_value(), "device recovery succeeds")) {
        return 16;
    }

    auto recreated = device->create_swapchain(desc);
    if (!expect(recreated.has_value(), "swapchain recreation after recovery")) {
        return 17;
    }
    swapchain = std::move(*recreated);
    native_swapchain = dynamic_cast<VulkanSwapchain*>(swapchain.get());
    auto recovered_pipeline_desc = tessera::rendering::vulkan::batched2d_pipeline_desc();
    recovered_pipeline_desc.color_format = swapchain->format();
    pipeline = device->create_pipeline(recovered_pipeline_desc);
    white = device->create_texture(white_desc);
    renderer = std::make_unique<tessera::rendering::gpu::Batched2DRenderer>(
        *device, *pipeline, *white, tessera::rendering::gpu::Batched2DConfig{4096, true, true});

    auto recovered_image = swapchain->acquire_next_image(1000);
    if (!expect(recovered_image.has_value(), "acquire after recovery") ||
        !expect(tessera::test::render_swapchain_frame(*device, *renderer, *swapchain,
                                                      recovered_image.value()),
                "render after recovery") ||
        !expect(present_ok(*swapchain, recovered_image.value()), "present after recovery")) {
        return 18;
    }
#endif

    // Resize the native window, then tell the swapchain; the first acquire retires the old
    // swapchain and reports OutOfDate while adopting the pending extent.
    if (!expect(resize_client(hwnd, 400, 300), "SetWindowPos resize succeeds")) {
        return 9;
    }
    pump_messages();
    const tessera::hal::WindowMetrics resized = window->metrics();
    if (!expect(resized.physical_width == 400 && resized.physical_height == 300,
                "resized client size")) {
        return 10;
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
        return 11;
    }

    auto adopted = swapchain->acquire_next_image(1000);
    if (!expect(adopted.has_value(), "acquire adopts the new swapchain")) {
        return 12;
    }
    if (!expect(
            tessera::test::render_swapchain_frame(*device, *renderer, *swapchain, adopted.value()),
            "render after resize")) {
        return 13;
    }
    if (!expect(present_ok(*swapchain, adopted.value()), "present after resize succeeds")) {
        return 13;
    }

    const std::uint32_t swapchains_created = native_swapchain->stats().swapchains_created;
    const std::uint32_t presents = native_swapchain->stats().presents;

    // Destroy in dependency order: renderer/pipeline/texture, then swapchain (surface and views),
    // then the device, then the window.
    renderer.reset();
    white.reset();
    pipeline.reset();
    swapchain.reset();
    device.reset();
    window.reset();

    std::cout << "vulkan_swapchain: ok (" << swapchains_created << " swapchains, " << presents
              << " presents, format=" << format_name(chosen_format)
              << (srgb_swapchain ? " (sRGB)" : " (UNORM fallback)")
              << ", alpha=" << (alpha_supported ? "premultiplied" : "opaque") << ")\n";
    return 0;
}
