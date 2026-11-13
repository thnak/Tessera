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
    // Asks for a per-pixel-alpha composite mode so a translucent window's client area reaches the
    // compositor (glass-v1.md section 6). It is a request: a surface that offers no such mode
    // presents opaque, and the caller reads Swapchain::alpha_mode() to decide whether translucency
    // was obtained.
    bool per_pixel_alpha = false;
};

// The composite mode the surface actually got. `PreMultiplied` is the per-pixel-alpha mode the
// engine requires for a translucent window; `Opaque` is the documented fallback.
enum class SwapchainAlphaMode : std::uint8_t {
    Opaque = 0,
    PreMultiplied = 1,
};

// Frame-pacing counters of a swapchain that gates slot reuse on GPU completion (see
// docs/design/gpu/frames.md section 5.1). They are optional: a backend that does not implement
// timeline pacing leaves the default zeroed snapshot, so the RHI contract does not require it to
// change. `timeline_value` is the completion value the GPU has reached, `frames_in_flight` counts
// submitted frames whose value is not reached yet, and `slots_skipped` counts acquires that found
// no reusable slot and reported Timeout instead of waiting.
struct SwapchainFrameSync {
    std::uint64_t timeline_value = 0;
    std::uint32_t frames_in_flight = 0;
    std::uint32_t slots_skipped = 0;
};

enum class SwapchainError {
    OutOfDate,
    Timeout,
    SurfaceLost,
    NotSupported,
    // The operation completed (the image was acquired or presented) but the surface no longer
    // matches the swapchain; recreate it at the next frame boundary.
    Suboptimal,
    // The device was lost while acquiring/presenting. Stop presenting, release the swapchain and
    // RHI resources, then call Device::recover(); the swapchain must be recreated.
    DeviceLost,
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

    // The composite mode in force for this swapchain generation (glass-v1.md section 6). A caller
    // that requested `per_pixel_alpha` and reads `Opaque` must apply its documented opaque fallback
    // (the engine reports TESSERA_GLASS_WINDOW_UNSUPPORTED once). Backends without a presentation
    // surface answer Opaque.
    [[nodiscard]] virtual SwapchainAlphaMode alpha_mode() const {
        return SwapchainAlphaMode::Opaque;
    }

    // True when this surface offers a per-pixel-alpha composite mode, so a `per_pixel_alpha`
    // request can be honored. It is a creation-time capability of the surface, and the value stays
    // valid across a resize/recreation while the surface does.
    [[nodiscard]] virtual bool supports_per_pixel_alpha() const {
        return false;
    }

    // Returns the index of an image ready to render. timeout_ms == 0 polls; real backends clamp
    // larger values to a bounded internal wait so the UI thread never blocks indefinitely, and
    // report Timeout. The null backend never blocks. On Timeout the caller retries on a later
    // tick. A backend that paces frames with a completion counter reports Timeout when every frame
    // slot is still in flight, so the UI thread skips that tick instead of waiting for the GPU.
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

    // True when a smaller render target can be resampled into this swapchain's images with
    // CommandList::blit_target, which is what the adaptive render scale (docs/design/performance.md
    // section 6) presents through. It is a creation-time capability of the images themselves (a
    // surface/format transfer-destination and blit support), so a consumer queries it once and
    // keeps every frame on the full-size direct path when it is false.
    [[nodiscard]] virtual bool supports_scaled_present() const {
        return false;
    }

    // Completion counter snapshot for the frame-pacing gate. It is a plain host query (no GPU
    // sync), so a driver may sample it once per iteration to prove multi-frame overlap; backends
    // without timeline pacing answer a zeroed snapshot.
    [[nodiscard]] virtual SwapchainFrameSync frame_sync() const {
        return {};
    }
};

} // namespace tessera::rendering::rhi
