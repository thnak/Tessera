// SPDX-License-Identifier: MIT
//
// Swapchain surface-format preference (plain data, no GPU). A presentation surface is UNORM: the
// paint pipeline emits sRGB-encoded values and blends in that encoded space, so an `*_SRGB` image
// would encode every color a second time (issue #791). The sRGB siblings stay as a fallback for a
// surface that offers nothing else. The Vulkan-specific mapping from VkFormat to this plain model
// is exercised by the real-surface tests on a GPU host.

#include "vulkan_composite_alpha.hpp"
#include "vulkan_surface_format.hpp"

#include <array>
#include <cstddef>
#include <iostream>
#include <optional>
#include <span>

namespace {

using tessera::rendering::rhi::TextureFormat;
using tessera::rendering::vulkan::choose_composite_alpha;
using tessera::rendering::vulkan::choose_surface_format;
using tessera::rendering::vulkan::CompositeAlpha;
using tessera::rendering::vulkan::CompositeAlphaSupport;
using tessera::rendering::vulkan::SurfaceColorSpace;
using tessera::rendering::vulkan::SurfaceFormat;
using tessera::rendering::vulkan::SurfaceFormatCandidate;
using tessera::rendering::vulkan::to_rhi_format;

int failures = 0;

void expect_composite(const char* name, const CompositeAlphaSupport& support, bool per_pixel_alpha,
                      CompositeAlpha expected) {
    const CompositeAlpha actual = choose_composite_alpha(support, per_pixel_alpha);
    if (actual != expected) {
        std::cerr << "FAIL composite alpha " << name << "\n";
        ++failures;
    }
}

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

    // An UNORM-only surface picks the UNORM format, as before.
    expect_choice("unorm nonlinear", std::array{candidate(B8G8R8A8Unorm)}, std::size_t{0});

    // The regression of issue #791: UNORM wins even when the driver lists an sRGB sibling first,
    // because the paint pipeline's output is already sRGB-encoded.
    expect_choice("unorm beats earlier srgb",
                  std::array{candidate(B8G8R8A8Srgb), candidate(B8G8R8A8Unorm)}, std::size_t{1});
    expect_choice("r8 unorm beats r8 srgb",
                  std::array{candidate(R8G8B8A8Srgb), candidate(R8G8B8A8Unorm)}, std::size_t{1});
    // Within UNORM, the layout order applies (R8G8B8A8 before B8G8R8A8).
    expect_choice("unorm layout order",
                  std::array{candidate(B8G8R8A8Unorm), candidate(R8G8B8A8Unorm)}, std::size_t{1});
    // R8G8B8A8 UNORM is preferred over B8G8R8A8 UNORM even with an sRGB candidate between them.
    expect_choice(
        "unorm over both srgb siblings",
        std::array{candidate(B8G8R8A8Srgb), candidate(R8G8B8A8Unorm), candidate(B8G8R8A8Unorm)},
        std::size_t{1});

    // The color space still dominates the format preference: an sRGB format advertised with the
    // sRGB non-linear color space is chosen over a UNORM format paired with an unusual color space,
    // because the latter may not be presentable.
    expect_choice(
        "srgb color space beats other-space unorm",
        std::array{candidate(B8G8R8A8Srgb), candidate(R8G8B8A8Unorm, SurfaceColorSpace::Other)},
        std::size_t{0});
    // A preferred format is still chosen when its color space is unusual, rather than failing.
    expect_choice("unorm other-space fallback",
                  std::array{candidate(R8G8B8A8Unorm, SurfaceColorSpace::Other)}, std::size_t{0});
    // A surface that advertises only an sRGB format still gets a usable swapchain.
    expect_choice("srgb-only surface falls back to srgb", std::array{candidate(R8G8B8A8Srgb)},
                  std::size_t{0});
    expect_choice("bgra srgb-only surface falls back to srgb", std::array{candidate(B8G8R8A8Srgb)},
                  std::size_t{0});

    // A lone VK_FORMAT_UNDEFINED entry ("any format works") is handed back to the caller, which
    // then chooses UNORM for presentation.
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

    // Composite alpha (docs/specs/glass-v1.md section 6): a translucent window requests
    // premultiplied alpha, an opaque window keeps opaque, and a surface without the requested mode
    // degrades instead of failing the swapchain creation.
    {
        CompositeAlphaSupport all;
        all.pre_multiplied = true;
        all.post_multiplied = true;
        all.inherit = true;
        expect_composite("opaque default", all, false, CompositeAlpha::Opaque);
        expect_composite("per-pixel prefers premultiplied", all, true,
                         CompositeAlpha::PreMultiplied);

        CompositeAlphaSupport opaque_only;
        expect_composite("per-pixel degrades to opaque", opaque_only, true, CompositeAlpha::Opaque);

        CompositeAlphaSupport premultiplied_only;
        premultiplied_only.opaque = false;
        premultiplied_only.pre_multiplied = true;
        expect_composite("opaque request falls back to premultiplied", premultiplied_only, false,
                         CompositeAlpha::PreMultiplied);
        expect_composite("alpha request picks premultiplied", premultiplied_only, true,
                         CompositeAlpha::PreMultiplied);

        CompositeAlphaSupport post_multiplied_only;
        post_multiplied_only.opaque = false;
        post_multiplied_only.post_multiplied = true;
        expect_composite("alpha request falls back to post-multiplied", post_multiplied_only, true,
                         CompositeAlpha::PostMultiplied);
        expect_composite("opaque request falls back to post-multiplied", post_multiplied_only,
                         false, CompositeAlpha::PostMultiplied);

        CompositeAlphaSupport inherit_only;
        inherit_only.opaque = false;
        inherit_only.inherit = true;
        expect_composite("last resort is inherit", inherit_only, false, CompositeAlpha::Inherit);
    }

    if (failures != 0) {
        std::cerr << "vulkan_surface_format: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "vulkan_surface_format: ok\n";
    return 0;
}
