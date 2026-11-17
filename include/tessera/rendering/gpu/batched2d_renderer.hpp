// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/gpu/clip.hpp"
#include "tessera/rendering/gpu/clip_state_table.hpp"
#include "tessera/rendering/gpu/glass_blur.hpp"
#include "tessera/rendering/gpu/glass_compositor.hpp"
#include "tessera/rendering/gpu/vertex2d.hpp"
#include "tessera/rendering/gpu/vertex_arena.hpp"
#include "tessera/rendering/redraw_plan.hpp"
#include "tessera/rendering/rhi/device.hpp"

#include <cstdint>
#include <vector>

namespace tessera::rendering::gpu {

struct Batched2DConfig {
    std::uint32_t initial_vertex_capacity = 4096;
    bool enable_aa = true;
    bool golden_mode = false;
    // MSAA opt-in: 1 (default) or 4. Must equal the render target's sample count and the pipeline's
    // sample count; the renderer rejects a mismatch instead of rendering with the wrong pipeline.
    // A 4x target resolves into its 1-sample image at end of pass; the batched2d path itself is
    // sample-count agnostic (SDF rounding and coverage text keep working unchanged).
    std::uint32_t sample_count = 1;

    // Number of frames whose vertex data may be alive on the GPU at once. Each frame writes into
    // its own buffer slot, used round-robin. With the default of 1 the caller finishes a frame
    // (blocking submit() or waiting on the list) before beginning the next. With N > 1 the caller
    // must not begin frame k + N until frame k's command list is complete, because that frame
    // reuses frame k's slot.
    std::uint32_t frames_in_flight = 1;

    // Bounded offscreen targets for the isolated-layer seam (break_pass() and
    // acquire_offscreen_target(), docs/design/gpu/offscreen-layers.md). This is
    // `glassMaxBlurTargets` from docs/specs/glass-v1.md section 5: 4 by default, 0 disables the
    // pool. The pool never grows past it and recycles a target only after frames_in_flight frames.
    std::uint32_t max_offscreen_targets = 4;

    // Glass cost limits (glass-v1 sections 5 and 8), the same runtime keys the paint pass uses:
    // `max_blur_radius` is `glassMaxBlurRadius` (CSS px, clamped again here so a caller cannot make
    // the renderer blur further than the paint pass's damage region) and `max_backdrop_area` is
    // `glassMaxBackdropArea` (the half-resolution threshold).
    GlassLimits glass{};

    // CPU culling (issue #474): a quad whose transformed bounds cannot touch the active scissor or
    // the frame's damage regions is never staged, so its vertices are neither built nor uploaded.
    // The pixels are identical either way (the GPU scissor would have discarded the same
    // fragments); the switch exists to compare the two when debugging. On by default.
    bool cull_draws = true;
};

// Per-frame culling counters, reset by every begin. `quads_emitted` counts quads that reached the
// vertex buffer; the two culled counters are quads dropped before it.
struct CullStats {
    std::uint32_t quads_emitted = 0;
    std::uint32_t quads_culled = 0;               // outside the scissor or the damage regions
    std::uint32_t quads_culled_empty_scissor = 0; // dropped because the scissor has no area
};

// The carved-out box of a box shadow in local px: centre relative to the quad centre and half size.
// All zero subtracts nothing (every primitive other than a shadow quad). Declared at namespace
// scope, not inside the renderer, so a default argument may use it (a nested class's default member
// initializers are not available while the enclosing class is still incomplete).
struct ShadowCarveBox {
    float x = 0;
    float y = 0;
    float half_width = 0;
    float half_height = 0;
};

// The batched 2D renderer. It owns the frame and pass lifecycle, the transform and scissor stacks
// and the emit calls; the state clusters that used to live inside it are collaborators
// (god-type audit slice 8, docs/architecture/god-type-audit-v1.md section 3.5):
//
//  - ClipStateTable holds the rounded-clip stack and its interned shader fold.
//  - VertexArena owns the per-frame vertex buffers.
//  - GlassCompositor plans and records the glass backdrop passes.
//
// The break/resume seam stays here, because it must flush mid-frame state.
class Batched2DRenderer {
public:
    // `glass` supplies the two pipelines a backdrop composite records (glass-v1 sections 3-5,
    // issue #244). Leaving it empty keeps every non-glass frame byte-identical and makes
    // draw_backdrop() a documented no-op that the caller counts as unsupported.
    Batched2DRenderer(rhi::Device& device, rhi::Pipeline& pipeline, rhi::Texture& white_texture,
                      Batched2DConfig config = {}, GlassPipelines glass = {});
    ~Batched2DRenderer();

    Batched2DRenderer(const Batched2DRenderer&) = delete;
    Batched2DRenderer& operator=(const Batched2DRenderer&) = delete;

    void begin(rhi::CommandList& commands, const rhi::RenderTarget& target,
               const rhi::RenderTargetDesc& desc);
    // Damage-aware entry point. A full-redraw plan is identical to begin(); a partial
    // plan clips every emitted draw to exactly one damage region (see redraw_plan.hpp).
    // An empty partial plan is a no-op: no render pass is opened and nothing is drawn.
    // `plan.scissors` must be disjoint; DamageAccumulator guarantees that.
    void begin_damaged(rhi::CommandList& commands, const rhi::RenderTarget& target,
                       const rhi::RenderTargetDesc& desc, const RedrawPlan& plan);
    void end();

    // --- Isolated-layer seam (docs/design/gpu/offscreen-layers.md, issue #242) -----------------
    // Ends the current render pass without finishing the frame: pending geometry is flushed, the
    // pass is closed, and the frame target/desc stay current. Between break_pass() and
    // resume_pass() the caller may record CommandList operations that need the pass closed - in
    // particular copy_target_region() into a pooled target (glass #244) or a full subtree layer
    // (#227). Draws must not be recorded while the pass is broken. Idempotent: a second
    // break_pass() before resume_pass() is a no-op.
    void break_pass();
    // Reopens the frame target's render pass after break_pass() with LoadOp::Load, so everything
    // drawn before the break (and any copy placed back into the target) is preserved, then rebinds
    // the pipeline, viewport, scissor and clip uniforms the pass had. Transform, clip, scissor and
    // content-scale state recorded before the break is preserved; a tree that never breaks a pass
    // records exactly the same commands as before this seam existed. No-op when the pass is not
    // broken.
    void resume_pass();
    [[nodiscard]] bool pass_broken() const {
        return pass_broken_;
    }

    // A pooled offscreen render target of at least `extent` and the frame target's format, for use
    // between break_pass() and resume_pass(). The pool is bounded by
    // Batched2DConfig::max_offscreen_targets (= glassMaxBlurTargets, default 4) and recycles a
    // target only after its frame is complete (frames_in_flight). Null when the pool is disabled or
    // every target is still in flight; the caller then paints that frame without the effect.
    [[nodiscard]] rhi::RenderTarget* acquire_offscreen_target(rhi::Extent2D extent);
    [[nodiscard]] std::uint32_t offscreen_target_count() const;

    // --- Glass backdrop composite (docs/specs/glass-v1.md sections 3-5, issue #244) -------------
    // Executes one glass node's backdrop entirely on this frame's command list, at the node's paint
    // position: the pass is broken (pending geometry flushed first), the node's 3-sigma dependency
    // region is copied out of the frame target into a pooled target, the horizontal Gaussian runs
    // into a second pooled target sampling that copy, the frame pass is resumed with LoadOp::Load,
    // and the vertical pass composites the result back through the node's rounded clip, with the
    // saturate/brightness/contrast matrix and the folded node opacity. Above
    // `Batched2DConfig::glass.max_backdrop_area` the blur target is half the region and the
    // vertical pass upsamples bilinearly.
    //
    // Returns false without recording anything when the effect cannot run this frame: no glass
    // pipelines or pool, a pass-through sigma, a frame target that cannot be read (a swapchain
    // image is not a copy source), or the bounded pool having no free target. The caller then
    // paints the node without the effect - the documented bounded-cost behavior, never a block and
    // never an exception on the frame path.
    [[nodiscard]] bool draw_backdrop(const GlassBackdropDraw& draw);

    // True when draw_backdrop() can execute: both glass pipelines are configured, the offscreen
    // pool is enabled and the sink is inside a frame. Callers use it to report an unsupported
    // backdrop draw instead of pretending it painted glass.
    [[nodiscard]] bool glass_supported() const;

    // Blur + composite draws recorded so far (diagnostics and the benchmark).
    [[nodiscard]] std::uint32_t glass_draws() const;

    void set_viewport(rhi::Extent2D extent);
    void set_scissor(const rhi::Scissor& scissor);
    void push_scissor(const rhi::Scissor& scissor);
    void pop_scissor();

    // Global output scale applied to every emitted position and every clip rect, per axis. It lets
    // one scene - laid out and painted in the window's presented pixel coordinates - be rendered
    // into a smaller target (the adaptive render scale of docs/design/performance.md section 6) and
    // resampled back up on present. It is not a device/render-scale change: glyphs keep the atlas
    // raster they were shaped at, and the SDF/AA radii stay in local (pre-scale) units, so a
    // downgrade only resamples pixels. `begin()` resets it to (1, 1), which is the byte-identical
    // full-quality path.
    void set_content_scale(float x, float y);

    // Rounded-rect clip stack. The scissor is still intersected with each clip rect (fast path),
    // and the up-to-`clip::kMaxShaderClips` active clips are folded into push-constant uniforms for
    // the analytic fragment test. Rect-only clips keep the scissor-only path. Deeper nesting (up to
    // `clip::kMaxClipDepth`) is folded conservatively into the innermost shader slot; push/pop must
    // pair (mixing with push_scissor is allowed but each pop matches its own push).
    void push_clip(const clip::RoundedRect& clip);
    void pop_clip();
    [[nodiscard]] std::uint32_t clip_depth() const;

    void set_transform(const rhi::Mat3& transform);
    void push_transform(const rhi::Mat3& transform);
    void pop_transform();

    void push_rect(const rhi::Rect& rect, rhi::Color color);
    void push_rounded_rect(const rhi::Rect& rect, rhi::Color color, float radius);
    // A rounded-rect outline: the area of the rounded box that is not inside the same box inset by
    // `width` on every side (inner radius `radius - width`, at least 0). One quad; the fragment
    // shader subtracts the inner signed distance, so the stroke follows the corners and is
    // anti-aliased on both edges. `width` is clamped so the inner box does not invert.
    void push_rounded_ring(const rhi::Rect& rect, rhi::Color color, float radius, float width);
    // A box shadow with a Gaussian edge of standard deviation `sigma` around the rounded `box`. One
    // quad covering the box grown by 2 sigma; the fragment shader evaluates the blurred coverage
    // from the box's signed distance, so the falloff is continuous instead of stepped. `carve` is
    // the element's border box: CSS paints an outer shadow only outside it (CSS Backgrounds 3
    // section 7.1.1), so the shader multiplies the coverage by 0 inside it. An empty `carve` paints
    // the whole Gaussian. `sigma` 0 draws the hard-edged shadow (`box-shadow: 4px 4px 0`) through
    // the same quad and carve, with a coverage step instead of the Gaussian.
    void push_soft_shadow(const rhi::Rect& box, const rhi::Rect& carve, rhi::Color color,
                          float radius, float sigma);
    // `coverage_boost` (1 to 2) scales the sampled alpha before blending; glyph runs use it.
    void push_texture(const rhi::Rect& rect, rhi::Texture& texture, const rhi::Rect& uv,
                      rhi::Color color, float coverage_boost = 1.0f);

    void flush();

    [[nodiscard]] std::uint32_t draw_calls() const {
        return draw_calls_;
    }
    [[nodiscard]] std::uint32_t vertex_count() const;
    [[nodiscard]] std::uint32_t pending_batches() const;
    [[nodiscard]] const CullStats& cull_stats() const {
        return cull_stats_;
    }

private:
    struct Batch {
        rhi::Scissor scissor;
        rhi::Texture* texture = nullptr;
        std::uint32_t clip_state = 0;
        std::uint32_t first_vertex = 0;
        std::uint32_t vertex_count = 0;
    };

    void begin_frame(rhi::CommandList& commands, const rhi::RenderTarget& target,
                     const rhi::RenderTargetDesc& desc,
                     const std::vector<rhi::Scissor>* damage_regions);
    // Re-binds the pipeline, viewport, scissor and folded clip uniforms the frame pass needs. A new
    // pass starts with no state, and a glass composite records its own pipeline/push constants in
    // the middle of one, so both resume_pass() and draw_backdrop() restore the frame state here.
    void bind_frame_state();
    void flush_batch();
    void emit_quad(const rhi::Rect& rect, const rhi::Rect& uv, rhi::Color color, float radius,
                   float mode, rhi::Texture* texture, const ShadowCarveBox& carve = {});
    // True when the quad's device-space bounds cannot touch any pixel the frame will draw.
    [[nodiscard]] bool cull_quad(const rhi::Rect& rect);
    void update_push_constants(std::uint32_t state_index);
    void apply_clip_state(const ClipStateTable::Update& update);
    static void resume_pass_callback(void* context);

    rhi::Device* device_ = nullptr;
    rhi::Pipeline* pipeline_ = nullptr;
    rhi::Texture* white_texture_ = nullptr;
    rhi::CommandList* commands_ = nullptr;

    // Isolated-layer seam state: the frame target the pass was opened on (kept so resume_pass() can
    // reopen it).
    const rhi::RenderTarget* frame_target_ = nullptr;
    rhi::RenderTargetDesc frame_desc_;
    std::uint64_t frame_index_ = 0;
    bool pass_broken_ = false;

    GlassCompositor glass_;
    ClipStateTable clips_;
    VertexArena vertices_;

    Batched2DConfig config_;
    rhi::Extent2D extent_;
    rhi::Scissor base_scissor_;
    rhi::Scissor scissor_;
    std::vector<rhi::Scissor> scissor_stack_;
    std::vector<rhi::Scissor> damage_regions_;
    // Bounding box of `damage_regions_` (the whole target for a full redraw). A quad outside it is
    // outside every region, so the per-region replay in flush() would draw nothing for it.
    rhi::Scissor damage_bounds_;
    CullStats cull_stats_;

    rhi::Mat3 transform_;
    std::vector<rhi::Mat3> transform_stack_;
    // Presented-pixel -> target-pixel factor for this frame; (1, 1) unless a caller downgraded the
    // render target. See set_content_scale().
    float content_scale_x_ = 1.0f;
    float content_scale_y_ = 1.0f;

    std::vector<Batch> batches_;
    Batch current_batch_;
    rhi::Texture* current_texture_ = nullptr;

    std::uint32_t draw_calls_ = 0;
    bool in_frame_ = false;
};

} // namespace tessera::rendering::gpu
