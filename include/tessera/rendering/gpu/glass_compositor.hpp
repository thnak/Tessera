// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/gpu/glass_blur.hpp"
#include "tessera/rendering/gpu/offscreen_target_pool.hpp"
#include "tessera/rendering/rhi/device.hpp"

#include <cstdint>
#include <memory>
#include <vector>

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

    // Isolated layers (issue #227) reuse the composite pipeline (glass_blur.frag mode 2) and the
    // pool: true when a layer can be rendered into a pooled target and composited into `frame`.
    // The composite pipeline must match the frame target's sample count, and a layer target is
    // always single-sampled, so a multisampled frame paints its groups flat.
    [[nodiscard]] bool layers_supported(const rhi::RenderTarget& frame_target) const;
    // Frame-independent half: the composite pipeline and the pool exist. A paint pass asks this
    // while planning, before a frame target is known; the sample-count half is checked per layer.
    [[nodiscard]] bool layers_configured() const {
        return glass_.composite != nullptr && pool_ != nullptr;
    }

    // Composites `layer` (a pooled target the renderer drew a group into, holding premultiplied
    // colour over a transparent clear) into the frame target, once per scissor in `scissors` (the
    // frame's clip intersected with the layer's box, and with each damage region when the frame is
    // damage-clipped), with `opacity` applied once to the group. Requires the frame pass to be
    // broken; the pass is reopened through the seam. Returns false, recording nothing, when
    // layers_supported() is false.
    [[nodiscard]] bool composite_layer(const FrameContext& frame, rhi::RenderTarget& layer,
                                       const std::vector<rhi::Scissor>& scissors, float opacity);

    // True when a layer can be masked: layers are supported and the `DestinationIn` pipeline exists
    // with a single-sample layout (issue #617).
    [[nodiscard]] bool masks_supported() const {
        return layers_configured() && glass_.mask != nullptr && glass_.mask->sample_count() == 1;
    }
    // Multiplies the layer target `frame.target` by the alpha of `mask` (a pooled target the
    // renderer drew the mask image into over a transparent clear), once per scissor. The frame pass
    // is reopened through the seam like composite_layer(). Returns false, recording nothing, when
    // masks_supported() is false or the frame target is multisampled.
    [[nodiscard]] bool mask_layer(const FrameContext& frame, rhi::RenderTarget& mask,
                                  const std::vector<rhi::Scissor>& scissors);

    // Layer filters (issue #617): a separable Gaussian over a layer target, reusing the glass blur
    // program. One blur is two passes, horizontal `source` -> `scratch`, then vertical
    // `scratch` -> `dest` (which may be `source` itself: the horizontal pass has finished reading
    // it). A drop-shadow's vertical pass runs through the `DestinationIn` mask pipeline over a
    // destination cleared to the tint, so `dest` becomes the blurred, offset alpha in the shadow
    // colour; the caller then composites the original layer over it.
    struct FilterPass {
        float sigma = 0.0f;    // device px
        float offset_x = 0.0f; // device px, drop-shadow only
        float offset_y = 0.0f;
        bool shadow = false;
        rhi::Color tint{0.0f, 0.0f, 0.0f, 1.0f}; // straight alpha, drop-shadow only
        rhi::Scissor first_region;               // what the horizontal pass writes
    };
    // True when a blur (or, with `shadow`, a drop-shadow) can run on a single-sample layer target.
    [[nodiscard]] bool filters_supported(bool shadow) const {
        return layers_configured() && glass_.blur != nullptr && glass_.blur->sample_count() == 1 &&
               (!shadow || masks_supported());
    }
    // Records both passes, each in its own render pass, outside any open pass (the caller broke
    // it). The vertical pass writes once per scissor. Returns false, recording nothing, when
    // filters_supported() is false.
    [[nodiscard]] bool blur_layer(const FrameContext& frame, rhi::RenderTarget& source,
                                  rhi::RenderTarget& scratch, rhi::RenderTarget& dest,
                                  const FilterPass& pass,
                                  const std::vector<rhi::Scissor>& scissors);

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
    // Samples `source` 1:1 into the frame target with `pipeline` (glass_blur.frag mode 2).
    void composite_with(rhi::Pipeline& pipeline, const FrameContext& frame,
                        rhi::RenderTarget& source, const std::vector<rhi::Scissor>& scissors,
                        float opacity);

    rhi::Device* device_ = nullptr;
    GlassPipelines glass_;
    GlassLimits limits_;
    std::uint32_t max_targets_ = 0;
    std::unique_ptr<OffscreenTargetPool> pool_;
    std::uint64_t frame_index_ = 0;
    std::uint32_t draws_ = 0;
};

} // namespace tessera::rendering::gpu
