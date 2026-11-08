// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"

#include <cstdint>
#include <expected>

namespace tessera::rendering::rhi {

struct SwapchainDesc {
    Extent2D extent;
    TextureFormat format = TextureFormat::R8G8B8A8Unorm;
    // Double buffering is the minimum useful swapchain; backends reject smaller counts.
    std::uint32_t image_count = 2;
    bool vsync = true;
};

enum class SwapchainError {
    OutOfDate,
    Timeout,
    SurfaceLost,
    NotSupported,
};

// Presentation seam. Acquire/present are recoverable hot-path operations and return errors as
// values; callers recreate the swapchain on OutOfDate/SurfaceLost instead of blocking.
class Swapchain {
public:
    virtual ~Swapchain() = default;

    // Current (adopted) size. After resize(), the previous extent stays current until the
    // OutOfDate acquire adopts the pending one.
    [[nodiscard]] virtual Extent2D extent() const = 0;
    [[nodiscard]] virtual TextureFormat format() const = 0;
    [[nodiscard]] virtual std::uint32_t image_count() const = 0;

    // Returns the index of an image ready to render. timeout_ms == 0 polls and
    // std::numeric_limits<std::uint64_t>::max() waits indefinitely on real backends; the null
    // backend never blocks. On Timeout the caller should retry on a later tick.
    [[nodiscard]] virtual std::expected<std::uint32_t, SwapchainError>
    acquire_next_image(std::uint64_t timeout_ms) = 0;

    // Presents an image previously returned by acquire_next_image. Presenting an index that is
    // not acquired (never acquired, already presented, out of range, or retired by resize) is
    // rejected as OutOfDate and retires nothing.
    [[nodiscard]] virtual std::expected<void, SwapchainError> present(std::uint32_t image) = 0;

    // Requests a new extent. The next acquire returns OutOfDate and adopts the pending extent so
    // the caller can recreate dependent targets before retrying. A same-size resize is a no-op.
    virtual void resize(Extent2D extent) = 0;
};

} // namespace tessera::rendering::rhi
