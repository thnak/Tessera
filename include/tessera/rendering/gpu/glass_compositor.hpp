// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/gpu/glass_blur.hpp"
#include "tessera/rendering/gpu/offscreen_target_pool.hpp"
#include "tessera/rendering/rhi/device.hpp"

#include <cstdint>
#include <memory>

namespace tessera::rendering::gpu {

// The glass backdrop composite of the batched 2D renderer (glass-v1 sections 3-5, #244), as an
// object instead of a renderer TU (god-type audit slice 8).
//
// One glass node runs entirely inside the frame that owns it: the dependency region is copied out
// of the frame target, a horizontal Gaussian writes a pooled blur target, and the vertical pass
// composites back through the node's rounded clip. Everything is recorded on the frame's own
// command list, so nothing waits and nothing blocks the UI thread.
//
// This class only plans and records. The renderer still owns the frame pass: it breaks the pass
// before calling compose() and resumes it afterwards, because that state belongs to the frame, not
// to the effect. Keeping the break outside is what lets a future isolated layer (#227) reuse the
// same seam.
class GlassCompositor {
public:
    // `glass` supplies the two pipelines; leaving it empty makes compose() a documented no-op that
    // the caller reports as unsupported. `max_targets` is glassMaxBlurTargets (0 disables the
    // pool).
    GlassCompositor(rhi::Device& device, GlassPipelines glass, std::uint32_t max_targets,
                    const GlassLimits& limits);

    GlassCompositor(const GlassCompositor&) = delete;
    GlassCompositor& operator=(const GlassCompositor&) = delete;

    // The frame's pass seam. The compositor breaks nothing itself: the caller breaks the pass, and
    // between the horizontal and the composite pass the frame pass must be reopened, so the caller
    // supplies that one operation. A plain function pointer plus context keeps the frame path free
    // of `std::function` allocation and of virtual dispatch.
    struct PassSeam {
        void* context = nullptr;
        void (*resume)(void* context) = nullptr;

        void reopen() const {
            if (resume != nullptr) {
                resume(context);
            }
        }
    };

    // What one glass node needs from the renderer's frame, passed per call so the compositor holds
    // no back-pointer into the renderer.
    struct FrameContext {
        rhi::CommandList* commands = nullptr;
        const rhi::RenderTarget* target = nullptr;
        rhi::RenderTargetDesc desc;
        rhi::Extent2D extent;
        rhi::Scissor scissor; // the frame's current scissor (damage and scroll clips)
        float content_scale_x = 1.0f;
        float content_scale_y = 1.0f;
        PassSeam seam;
    };

    // Starts a frame: rotates the bounded pool and counts nothing else.
    void begin_frame(std::uint64_t frame_index, std::uint32_t frames_in_flight,
                     rhi::TextureFormat format);

    // True when compose() can run: both pipelines and a pool are configured.
    [[nodiscard]] bool supported() const;

    // Plans and records one node's backdrop. Returns false without recording anything when the
    // effect cannot run this frame - a pass-through sigma, a frame target that cannot be read (a
    // swapchain image is not a copy source), or the bounded pool having no free target - so the
    // caller paints the node without the effect. Requires the frame pass to be broken already.
    [[nodiscard]] bool compose(const FrameContext& frame, const GlassBackdropDraw& draw);

    [[nodiscard]] rhi::RenderTarget* acquire(rhi::Extent2D extent);
    [[nodiscard]] std::uint32_t target_count() const {
        return pool_ != nullptr ? pool_->target_count() : 0;
    }
    // Composite draws recorded so far (diagnostics and the benchmark).
    [[nodiscard]] std::uint32_t draws() const {
        return draws_;
    }
    void reset_draws() {
        draws_ = 0;
    }

private:
    [[nodiscard]] bool can_read_target(const rhi::RenderTarget& target) const;

    rhi::Device* device_ = nullptr;
    GlassPipelines glass_;
    GlassLimits limits_;
    std::uint32_t max_targets_ = 0;
    std::unique_ptr<OffscreenTargetPool> pool_;
    std::uint64_t frame_index_ = 0;
    std::uint32_t draws_ = 0;
};

} // namespace tessera::rendering::gpu
