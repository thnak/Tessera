// SPDX-License-Identifier: MIT
//
// The swapchain composite-alpha preference, as plain data (no Vulkan headers), so the rule is
// testable without a device or a surface. Vulkan-specific mapping lives in vulkan_swapchain.cpp.
//
// `per_pixel_alpha` requests premultiplied alpha, which is what makes a translucent window's client
// area composite over the desktop or the OS backdrop (docs/specs/glass-v1.md section 6). A surface
// that does not offer it keeps the opaque mode; the caller reads Swapchain::alpha_mode() and
// applies the documented fallback.

#pragma once

#include <cstdint>

namespace tessera::rendering::vulkan {

enum class CompositeAlpha : std::uint8_t {
    Opaque,
    PreMultiplied,
    PostMultiplied,
    Inherit,
};

// The surface's `supportedCompositeAlpha` answer. The Vulkan specification guarantees at least one
// mode, so the default (opaque) is a safe model of "the surface said nothing".
struct CompositeAlphaSupport {
    bool opaque = true;
    bool pre_multiplied = false;
    bool post_multiplied = false;
    bool inherit = false;
};

// Picks the mode a swapchain is created with. A per-pixel-alpha request prefers premultiplied, then
// the post-multiplied mode (still per-pixel alpha), then opaque; an opaque request prefers opaque
// and only uses another mode when the surface does not offer it.
[[nodiscard]] inline CompositeAlpha choose_composite_alpha(const CompositeAlphaSupport& support,
                                                           bool per_pixel_alpha) noexcept {
    if (per_pixel_alpha) {
        if (support.pre_multiplied) {
            return CompositeAlpha::PreMultiplied;
        }
        if (support.post_multiplied) {
            return CompositeAlpha::PostMultiplied;
        }
    }
    if (support.opaque) {
        return CompositeAlpha::Opaque;
    }
    if (support.pre_multiplied) {
        return CompositeAlpha::PreMultiplied;
    }
    if (support.post_multiplied) {
        return CompositeAlpha::PostMultiplied;
    }
    if (support.inherit) {
        return CompositeAlpha::Inherit;
    }
    return CompositeAlpha::Opaque;
}

} // namespace tessera::rendering::vulkan
