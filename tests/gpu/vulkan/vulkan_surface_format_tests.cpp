// SPDX-License-Identifier: MIT
//
// D2 swapchain surface-format preference (plain data, no GPU): sRGB-encoded formats are preferred
// with an sRGB non-linear color space, with the historical UNORM selection as fallback. The
// Vulkan-specific mapping from VkFormat to this plain model is exercised by the real-surface
// tests on a GPU host.

#include "vulkan_surface_format.hpp"

#include <array>
#include <cstddef>
#include <iostream>
#include <optional>
#include <span>

namespace {

using tessera::rendering::rhi::TextureFormat;
using tessera::rendering::vulkan::choose_surface_format;
using tessera::rendering::vulkan::SurfaceColorSpace;
using tessera::rendering::vulkan::SurfaceFormat;
using tessera::rendering::vulkan::SurfaceFormatCandidate;
using tessera::rendering::vulkan::to_rhi_format;

int failures = 0;

void expect_choice(const char* name, std::span<const SurfaceFormatCandidate> formats,
                   std::optional<std::size_t> expected) {
    const std::optional<std::size_t> actual = choose_surface_format(formats);
    if (actual != expected) {
        std::cerr << "FAIL " << name << ": expected "
                  << (expected ? static_cast<long long>(*expected) : -1LL) << ", got "
                  << (actual ? static_cast<long long>(*actual) : -1LL) << "\n";
        ++failures;
    }
}

void expect_rhi(const char* name, SurfaceFormat format, TextureFormat expected) {
    if (to_rhi_format(format) != expected) {
        std::cerr << "FAIL " << name << ": wrong RHI format\n";
        ++failures;
    }
}

constexpr SurfaceFormatCandidate
candidate(SurfaceFormat format, SurfaceColorSpace color_space = SurfaceColorSpace::SrgbNonlinear) {
    return SurfaceFormatCandidate{format, color_space};
}

} // namespace

int main() {
    using enum SurfaceFormat;

    // No usable format: the caller reports NotSupported.
    expect_choice("empty", std::span<const SurfaceFormatCandidate>{}, std::nullopt);
    expect_choice("unsupported format only", std::array{candidate(Other)}, std::nullopt);

    // An UNORM-only surface keeps the pre-D2 UNORM selection.
    expect_choice("unorm nonlinear", std::array{candidate(B8G8R8A8Unorm)}, std::size_t{0});

    // sRGB wins even when the driver lists its UNORM sibling first.
    expect_choice("srgb beats earlier unorm",
                  std::array{candidate(B8G8R8A8Unorm), candidate(R8G8B8A8Srgb)}, std::size_t{1});
    // Within sRGB, the historical layout order still applies (R8G8B8A8 before B8G8R8A8).
    expect_choice("srgb layout order", std::array{candidate(B8G8R8A8Srgb), candidate(R8G8B8A8Srgb)},
                  std::size_t{1});
    // R8G8B8A8 sRGB is preferred over B8G8R8A8 sRGB even with an UNORM candidate between them.
    expect_choice(
        "srgb over both unorm siblings",
        std::array{candidate(B8G8R8A8Unorm), candidate(R8G8B8A8Srgb), candidate(B8G8R8A8Srgb)},
        std::size_t{1});

    // A non-sRGB color space does not beat a preferred UNORM format with an sRGB color space.
    expect_choice(
        "nonlinear unorm beats other-space srgb",
        std::array{candidate(B8G8R8A8Unorm), candidate(R8G8B8A8Srgb, SurfaceColorSpace::Other)},
        std::size_t{0});
    // But a preferred format is still chosen when its color space is unusual, rather than failing.
    expect_choice("srgb other-space fallback",
                  std::array{candidate(R8G8B8A8Srgb, SurfaceColorSpace::Other)}, std::size_t{0});
    expect_choice("unorm other-space fallback",
                  std::array{candidate(R8G8B8A8Unorm, SurfaceColorSpace::Other)}, std::size_t{0});

    // A lone VK_FORMAT_UNDEFINED entry ("any format works") is handed back to the caller.
    expect_choice("undefined nonlinear", std::array{candidate(Undefined)}, std::size_t{0});
    expect_choice("undefined other space",
                  std::array{candidate(Undefined, SurfaceColorSpace::Other)}, std::size_t{0});
    // Undefined is not chosen when the surface also advertises a preferred format.
    expect_choice("undefined among preferred",
                  std::array{candidate(Undefined), candidate(R8G8B8A8Unorm)}, std::size_t{1});

    const auto mapping_ok = to_rhi_format(R8G8B8A8Srgb) == TextureFormat::R8G8B8A8Srgb &&
                            to_rhi_format(B8G8R8A8Srgb) == TextureFormat::B8G8R8A8Srgb &&
                            to_rhi_format(R8G8B8A8Unorm) == TextureFormat::R8G8B8A8Unorm &&
                            to_rhi_format(B8G8R8A8Unorm) == TextureFormat::B8G8R8A8Unorm;
    if (!mapping_ok) {
        std::cerr << "FAIL surface format -> RHI texture format mapping\n";
        ++failures;
    }
    expect_rhi("r8 srgb", R8G8B8A8Srgb, TextureFormat::R8G8B8A8Srgb);
    expect_rhi("b8 srgb", B8G8R8A8Srgb, TextureFormat::B8G8R8A8Srgb);

    if (failures != 0) {
        std::cerr << "vulkan_surface_format: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "vulkan_surface_format: ok\n";
    return 0;
}
