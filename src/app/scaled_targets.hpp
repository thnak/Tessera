// SPDX-License-Identifier: MIT
//
// The adaptive render-scale targets of WindowHost::Impl, as a type (god-type audit slice 9, section
// 3.2).
//
// The host used to keep three members and one method for this: one offscreen target per frame slot,
// the extent each was allocated at, and the swapchain generation's answer to whether a smaller
// target can be resampled into the presented images. The policy is entirely "one target per slot,
// reused while the decided extent lasts", which has nothing to do with the window's event loop - so
// it is a type here, and the host only asks it for a target.
//
// One target per frame slot, never one shared image: a target may still be read by that slot's
// in-flight blit, so reusing a single image across two frames in flight would be a
// cross-submission read/write hazard. That is the same reason the renderer's vertex buffers rotate
// per slot, and the slot gate is what proves the previous read completed.

#pragma once

#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/rhi/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace tessera::app::detail {

namespace rhi = tessera::rendering::rhi;

// Two frames in flight: the render path never waits on the GPU, it skips a slot that is still busy
// and retries on the next tick (docs/design/window-resize.md section 0). Declared here because the
// target array is sized by it; window_impl.hpp asserts the two agree.
inline constexpr std::size_t kScaledTargetSlots = 2;

class ScaledTargets {
public:
    ScaledTargets() = default;

    ScaledTargets(const ScaledTargets&) = delete;
    ScaledTargets& operator=(const ScaledTargets&) = delete;

    // A new presentation generation: every target belongs to the old device and is released, and
    // the resample capability is re-read from the new swapchain.
    void reset(bool resample_supported);

    // Whether a smaller target can be resampled into this generation's presented images. Fixed for
    // a swapchain generation, so the frame path never re-queries it.
    [[nodiscard]] bool resample_supported() const {
        return resample_supported_;
    }

    // Ensures `slot` has a target of exactly `extent` in `format`, creating or reusing it, and
    // returns it. Null when the backend refuses or the request is empty; the caller then paints at
    // full quality rather than dropping the frame (ADR-025 boundary). `fallback` is set when a
    // creation attempt failed, so the host can count it without this type knowing about stats.
    [[nodiscard]] rhi::RenderTarget* ensure(rhi::Device& device, std::size_t slot,
                                            rhi::Extent2D extent, rhi::TextureFormat format,
                                            rhi::Color clear_color, bool& fallback);

    // Releases every target without touching the capability, for teardown of the images alone.
    void release();

    [[nodiscard]] rhi::RenderTarget* get(std::size_t slot) const;
    // The extent `slot`'s live target was allocated at; zero when it has none.
    [[nodiscard]] rhi::Extent2D extent(std::size_t slot) const;
    static constexpr std::size_t slot_count() {
        return kScaledTargetSlots;
    }

private:
    // The extent a slot's target holds; a mismatch means the target must be recreated, and the
    // format is compared against the live target itself (it is a property of the generation).
    struct Entry {
        std::unique_ptr<rhi::RenderTarget> target;
        rhi::Extent2D extent{};
    };

    bool resample_supported_ = false;
    std::array<Entry, kScaledTargetSlots> targets_{};
};

} // namespace tessera::app::detail
