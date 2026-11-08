// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"

#include <cstdint>
#include <expected>

namespace tessera::rendering::rhi {

class RenderTarget;

struct SwapchainDesc {
    Extent2D extent;
    TextureFormat format = TextureFormat::R8G8B8A8Unorm;
    // Double buffering is the minimum useful swapchain; backends reject smaller counts.
    std::uint32_t image_count = 2;
    bool vsync = true;
    // Opaque native presentation target supplied by the HAL/platform layer, never by UI code.
    // The handle is borrowed and must stay valid for the swapchain's lifetime. On Win32
    // `native_window` is the HWND; `native_display` is unused. On X11 `native_window` is the
    // Window id and `native_display` is the Display*.
    void* native_window = nullptr;
    void* native_display = nullptr;
};

enum class SwapchainError {
    OutOfDate,
    Timeout,
    SurfaceLost,
    NotSupported,
    // The operation completed (the image was acquired or presented) but the surface no longer
    // matches the swapchain; recreate it at the next frame boundary.
    Suboptimal,
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

    // Returns the index of an image ready to render. timeout_ms == 0 polls; real backends clamp
    // larger values to a bounded internal wait so the UI thread never blocks indefinitely, and
    // report Timeout. The null backend never blocks. On Timeout the caller retries on a later
    // tick.
    [[nodiscard]] virtual std::expected<std::uint32_t, SwapchainError>
    acquire_next_image(std::uint64_t timeout_ms) = 0;

    // Presents an image previously returned by acquire_next_image. Presenting an index that is
    // not acquired (never acquired, already presented, out of range, or retired by resize) is
    // rejected as OutOfDate and retires nothing. Suboptimal means the image was presented but
    // the swapchain should be recreated.
    [[nodiscard]] virtual std::expected<void, SwapchainError> present(std::uint32_t image) = 0;

    // Requests a new extent. The next acquire recreates the backend resources (retiring the old
    // swapchain), returns OutOfDate, and adopts the pending extent so the caller can recreate
    // dependent targets before retrying. A same-size resize is a no-op.
    virtual void resize(Extent2D extent) = 0;

    // Render target for a swapchain image. The pointer stays valid until the swapchain is
    // recreated; re-acquire after OutOfDate. Returns nullptr when the backend cannot present.
    [[nodiscard]] virtual RenderTarget* image_target(std::uint32_t index) = 0;
};

} // namespace tessera::rendering::rhi
