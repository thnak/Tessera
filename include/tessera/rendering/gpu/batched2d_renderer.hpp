// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/gpu/batch_split.hpp"
#include "tessera/rendering/gpu/batched2d_config.hpp"
#include "tessera/rendering/gpu/batched2d_cull_stats.hpp"
#include "tessera/rendering/gpu/clip.hpp"
#include "tessera/rendering/gpu/clip_buffer.hpp"
#include "tessera/rendering/gpu/clip_state_table.hpp"
#include "tessera/rendering/gpu/glass_blur.hpp"
#include "tessera/rendering/gpu/glass_compositor.hpp"
#include "tessera/rendering/gpu/layer_filter.hpp"
#include "tessera/rendering/gpu/layer_retention.hpp"
#include "tessera/rendering/gpu/vertex2d.hpp"
#include "tessera/rendering/gpu/vertex_arena.hpp"
#include "tessera/rendering/redraw_plan.hpp"
#include "tessera/rendering/rhi/device.hpp"

#include <cstdint>
#include <vector>

namespace tessera::rendering::gpu {

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
    // Batched2DConfig::max_offscreen_targets (= glassMaxBlurTargets, default 4). Null when it is
    // disabled or every target is live or in flight; the caller then paints without the effect.
    [[nodiscard]] rhi::RenderTarget* acquire_offscreen_target(rhi::Extent2D extent);
    // Returns a target once its last use is recorded, so this frame may reuse it.
    void release_offscreen_target(const rhi::RenderTarget* target);
    [[nodiscard]] std::uint32_t offscreen_target_count() const;
    // Pool state: targets, peak live, bytes and refused requests.
    [[nodiscard]] OffscreenPoolStats offscreen_stats() const;
    // Destroys the free pooled targets: call from an idle driver once every frame's GPU work is
    // done (`all_complete`). The frame path also trims on its own. Never call inside a frame.
    void trim_offscreen_targets(bool all_complete);

    // --- Glass backdrop composite (docs/specs/glass-v1.md sections 3-5, issue #244) -------------
    // Executes one glass node's backdrop at the node's paint position: break the pass, copy the
    // 3-sigma region into a pooled target, blur it horizontally into a second one, resume, and
    // composite the vertical pass through the node's rounded clip (color matrix, folded opacity).
    // Returns false, recording nothing, when the effect cannot run (no glass pipelines or pool, a
    // pass-through sigma, an unreadable frame target, no free pooled target): the caller paints the
    // node without it. Never blocks or throws on the frame path.
    [[nodiscard]] bool draw_backdrop(const GlassBackdropDraw& draw);

    // --- Isolated layers (docs/design/gpu/offscreen-layers.md section 5.2, issue #227) ----------
    // Renders everything between push_layer() and the matching pop_layer() into a pooled
    // frame-sized target, then composites it once with `opacity` on the group. `box` is the
    // group's paint-space bounds; the composite is limited to its device bounds, the frame scissor
    // and the damage regions. Layers nest and must pair. `filter` (issue #617) runs on the layer
    // first (before the mask and the opacity). A layer that cannot be isolated (no composite
    // pipeline or pool, a multisampled frame, no free target, an empty box) paints flat: its
    // opacity multiplies into every primitive (paint-pass.md 14.1) and its filter is skipped.
    void push_layer(const rhi::Rect& box, float opacity, const LayerFilterChain& filter = {});
    void pop_layer();
    // Masks the innermost open isolated layer (issue #617): begin_layer_mask() redirects drawing to
    // a pooled target cleared to transparent, and end_layer_mask() multiplies the layer by its
    // alpha (RHI 1.19 `DestinationIn`). Call both after the layer's content, before pop_layer().
    // False records nothing (flat or masked layer, no pipeline or free target): skip the mask draws
    // and end_layer_mask(); counted in layer_masks_unsupported().
    [[nodiscard]] bool begin_layer_mask();
    void end_layer_mask();
    // A non-`add` `mask-composite` layer inside an open mask: drawn into its own target, then
    // combined with the mask so far. False records nothing; drop it and skip end_mask_layer().
    [[nodiscard]] bool begin_mask_layer(MaskCompositeOp op);
    void end_mask_layer();
    [[nodiscard]] std::uint32_t layer_masks_applied() const {
        return layer_masks_applied_;
    }
    [[nodiscard]] std::uint32_t layer_masks_unsupported() const {
        return layer_masks_unsupported_;
    }
    [[nodiscard]] std::uint32_t layer_filters_applied() const {
        return layer_filters_applied_;
    }
    [[nodiscard]] std::uint32_t layer_filters_unsupported() const {
        return layer_filters_unsupported_;
    }
    // True when layers can be isolated at all (composite pipeline and pool).
    [[nodiscard]] bool layers_supported() const;
    // Retained layers (offscreen-layers-retention.md). Call right before push_layer() with the
    // node's `owner` id, the `key` every input of its pixels is folded into, and whether the node
    // and subtree are `clean`. True: the cache holds the finished pixels, push_layer()/pop_layer()
    // composite them and nothing between may be drawn. False: draw as usual; a clean node is cached
    // at pop_layer(), a dirty one drops its entry. Never throws.
    [[nodiscard]] bool retain_layer(std::uint64_t owner, std::uint64_t key, bool clean);
    // Not final (image loading, glass that could not run): the open layer is not retained.
    void note_layer_degraded() {
        ++retain_.degraded;
    }
    [[nodiscard]] RetainedLayerStats retained_layer_stats() const;
    // Layers of this frame isolated / painted flat.
    [[nodiscard]] std::uint32_t layers_isolated() const {
        return layers_isolated_;
    }
    [[nodiscard]] std::uint32_t layers_flat() const {
        return layers_flat_;
    }

    // True when draw_backdrop() can execute (glass pipelines, pool, inside a frame).
    [[nodiscard]] bool glass_supported() const;

    // Blur + composite draws recorded so far.
    [[nodiscard]] std::uint32_t glass_draws() const;

    // --- Texture arrays (RHI 1.7, issue #480 part 2) -------------------------------------------
    // Supplies the descriptor-indexing pipeline (`vulkan::batched2d_array_pipeline_desc`, only on a
    // device whose `texture_array_capacity()` is non-zero). Null means one set per texture. With
    // one, a batch may hold quads over up to that many textures, each vertex carrying its slot, so
    // interleaved images and glyph pages share a draw; pixels are identical either way. Set between
    // frames.
    void set_texture_array_pipeline(rhi::Pipeline* pipeline);
    [[nodiscard]] bool texture_array_supported() const {
        return array_pipeline_ != nullptr;
    }

    void set_viewport(rhi::Extent2D extent);
    // Scissors are in target pixels: `begin()` sets the frame scissor from the target extent and
    // `begin_damaged()`'s regions are already device pixels. A clip rect, which is authored in
    // paint (CSS) coordinates, must go through push_clip() instead, because that is where the
    // frame's content scale is applied before the scissor is derived from it.
    void set_scissor(const rhi::Scissor& scissor);
    void push_scissor(const rhi::Scissor& scissor);
    void pop_scissor();

    // Global output scale applied to every emitted position and every clip rect, per axis. It maps
    // paint (CSS) coordinates to the frame target's pixels: the window composes its device scale
    // with the adaptive render-scale factor (ui::scale::content_scale_for), so one scene reaches a
    // target that is larger than its logical size (a hi-DPI window) or smaller (a downgraded frame
    // resampled back up on present). Glyph rasterization keeps the atlas it was shaped at and the
    // SDF/AA radii stay in local (pre-scale) units, so this scales geometry, never the raster.
    // `begin()` resets it to (1, 1), which is the byte-identical 1.0 path.
    void set_content_scale(float x, float y);

    // Rounded-rect clip stack. The scissor is still intersected with each clip rect (fast path),
    // and the up-to-`clip::kMaxShaderClips` active clips are folded into push-constant uniforms for
    // the analytic fragment test. Rect-only clips keep the scissor-only path. Deeper nesting (up to
    // `clip::kMaxClipDepth`) is folded conservatively into the innermost shader slot; push/pop must
    // pair (mixing with push_scissor is allowed but each pop matches its own push).
    void push_clip(const clip::RoundedRect& clip);
    // Pushes a shaped clip (ADR-068 slice 5): a circle/ellipse, an elliptical-radius rounded rect
    // or a polygon, in paint coordinates like push_clip(). It shares the stack and the pop with
    // push_clip(), so the two nest freely. A polygon over the vertex limits degrades to its
    // bounding box (see ClipStateTable::push_shape).
    void push_clip_shape(const clip::ShapeClip& clip);
    void pop_clip();
    [[nodiscard]] std::uint32_t clip_depth() const;
    // Shaped clips this frame that degraded to their bounding box because a polygon was over a
    // vertex limit; zero when every shape drew as authored.
    [[nodiscard]] std::uint32_t clip_shape_fallbacks() const {
        return clips_.shape_fallbacks();
    }

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
    // An inset box shadow (issue #612): the same Gaussian edge, painted *inside* `box` instead of
    // outside `carve`. `box` is the element's border box (the quad the shadow is clipped to) and
    // `carve` is the inner box - the border box moved by the shadow's offset and shrunk by its
    // spread - so the shadow fills the ring between them and fades inward. The inner corner radius
    // is `radius` minus the spread, derived from the two boxes because an engine `border-radius` is
    // one length. `sigma` 0 is the hard-edged inset shadow.
    void push_inset_shadow(const rhi::Rect& box, const rhi::Rect& carve, rhi::Color color,
                           float radius, float sigma);
    // `coverage_boost` (1 to 2) scales the sampled alpha before blending; glyph runs use it.
    void push_texture(const rhi::Rect& rect, rhi::Texture& texture, const rhi::Rect& uv,
                      rhi::Color color, float coverage_boost = 1.0f);
    // Draws the whole texture into `rect` with integer-exact nearest sampling (#682): each device
    // pixel maps to texel `min(((2 * p + 1) * src) / (2 * dst), src - 1)` per axis, computed in the
    // fragment shader, so the result is bit-identical on every GPU and to the CPU rule
    // (`CpuCanvas::blit_texture`). The hardware nearest sampler is not: it picks the wrong neighbor
    // on every pixel of a 2:1 downscale. `rect` should land on whole device pixels under the active
    // transform and content scale (a fractional edge is rounded). The texture's own sampler state
    // is ignored, except under a rotated, skewed or mirrored transform: that has no integer device
    // rectangle, so the quad falls back to `push_texture` with the texture's sampler.
    void push_texture_exact(const rhi::Rect& rect, rhi::Texture& texture, rhi::Color color);

    // --- Per-fragment gradients (docs/design/gpu/gradients.md, ADR-076) -------------------------
    // A gradient layer is ONE quad: the fragment shader evaluates the linear, radial or conic ramp
    // exactly at every pixel centre from a record in the frame record buffer, so the cost of a
    // gradient no longer scales with how finely it was tessellated. The quad batches like any other
    // (texture, scissor and clip set are unchanged), and it honours the active transform, clip
    // stack, content scale, flat-layer opacity and damage culling like a solid quad.
    //
    // Returns false, recording nothing, when the gradient cannot be drawn this way: the device does
    // not report `supports_analytic_gradients()`, the frame pipeline has no record buffer (a test
    // fixture), or the frame's records or stops are full. The caller then paints the banded
    // approximation instead, which is the capability fallback; every refusal is counted in
    // gradient_fallbacks(). A culled or out-of-frame gradient returns true: there is nothing to
    // draw.
    [[nodiscard]] bool push_gradient(const gradient::Fill& fill);
    [[nodiscard]] bool gradients_supported() const;
    // Gradient quads emitted and gradients refused (so painted banded) this frame.
    [[nodiscard]] std::uint32_t gradient_quads() const {
        return gradient_quads_;
    }
    [[nodiscard]] std::uint32_t gradient_fallbacks() const {
        return gradient_fallbacks_;
    }

    // --- LCD text (docs/design/display-aware-rendering.md section 3.4.1, step 3) ----------------
    // Supplies the dual-source pipeline LCD glyph quads are drawn with. It must be created from
    // `vulkan::batched2d_lcd_pipeline_desc` with the frame's format and sample count, and only on a
    // device whose `supports_dual_source_blend()` is true. Null (the default) means "no LCD": every
    // run composites through the grayscale pipeline. Set between frames, like the other policy.
    void set_lcd_pipeline(rhi::Pipeline* pipeline);
    [[nodiscard]] bool lcd_supported() const {
        return lcd_pipeline_ != nullptr;
    }
    // A glyph quad over an LCD atlas page, drawn with the LCD pipeline. An LCD page keeps coverage
    // in RGB (and the strongest channel in alpha), so the ordinary pipeline - which multiplies the
    // ink by the texel's RGB - cannot draw it: every glyph of an LCD atlas goes through here.
    // `per_channel` composites each channel with its own coverage (an eligible run); false reads
    // the page's alpha as one coverage for all three, which is what a grayscale run, and a glyph
    // FreeType could only render grayscale, need. The quad is its own batch identity: it never
    // merges with a grayscale quad over the same texture. Without an LCD pipeline this is
    // `push_texture`, correct only for a grayscale page; an LCD atlas exists only where the
    // pipeline does.
    void push_texture_lcd(const rhi::Rect& rect, rhi::Texture& texture, const rhi::Rect& uv,
                          rhi::Color color, float coverage_boost = 1.0f, bool per_channel = true);

    // --- 4x presentation (docs/design/display-aware-rendering.md section 4, S8) ----------------
    // Draws `source`'s readable image into `destination` as one full-target textured quad with
    // `pipeline`, the GPU-only way to present a multisampled frame (`blit_target` takes 1-sample
    // sources only). The quad stretches the source with its own sampler state, which also covers
    // an adaptive-scale downgrade. Call after end() (RHI 1.3: the sampled bind is outside a pass).
    // `pipeline` must be single-sampled and match `destination`/`destination_desc`, which carries
    // the frame's clear color; `source` must be non-presentable and hold rendered content.
    // Contract violations throw (std::logic_error/std::invalid_argument), like begin().
    void present_target(rhi::CommandList& commands, const rhi::RenderTarget& destination,
                        const rhi::RenderTargetDesc& destination_desc,
                        const rhi::RenderTarget& source, rhi::Pipeline& pipeline);

    void flush();

    [[nodiscard]] std::uint32_t draw_calls() const {
        return draw_calls_;
    }
    [[nodiscard]] std::uint32_t vertex_count() const;
    [[nodiscard]] std::uint32_t pending_batches() const;
    [[nodiscard]] const ClipBuffer& clip_buffer() const {
        return clip_buffer_;
    }
    [[nodiscard]] const CullStats& cull_stats() const {
        return cull_stats_;
    }
    // Why the frame's batches ended (issue #480 part 2). `texture` is the share descriptor indexing
    // would remove; the rest are structural.
    [[nodiscard]] const BatchSplitStats& batch_split_stats() const {
        return split_stats_;
    }

private:
    struct Batch {
        rhi::Scissor scissor;
        rhi::Texture* texture = nullptr;
        std::uint32_t clip_state = 0;
        std::uint32_t first_vertex = 0;
        std::uint32_t vertex_count = 0;
        // Null draws with the frame pipeline; set only for LCD glyph quads.
        rhi::Pipeline* pipeline = nullptr;
        // True when this batch's vertices carry a frame-array index (RHI 1.7, issue #480 part 2),
        // so the flush binds the frame's array instead of this batch's single texture.
        bool texture_array = false;
    };

    void begin_frame(rhi::CommandList& commands, const rhi::RenderTarget& target,
                     const rhi::RenderTargetDesc& desc,
                     const std::vector<rhi::Scissor>* damage_regions);
    // Re-binds the pipeline, viewport, scissor and folded clip uniforms the frame pass needs: a new
    // pass starts with no state, so resume_pass() and every effect restore it here.
    void bind_frame_state();
    // `reason` says why the batch ended; it is counted in split_stats_ (issue #480 part 2).
    void flush_batch(BatchSplitReason reason);
    // Records one batch split in split_stats_ (issue #480 part 2).
    void count_batch_split(BatchSplitReason reason);
    // Applies the batch key for one quad's texture and returns the index its vertices carry (the
    // frame-array slot, or 0 on the one-set-per-texture path); ends the batch on a key change.
    std::uint32_t begin_quad_batch(rhi::Texture* texture);
    void emit_quad(const rhi::Rect& rect, const rhi::Rect& uv, rhi::Color color, float radius,
                   float mode, rhi::Texture* texture, const ShadowCarveBox& carve = {});
    // True when the quad's device-space bounds cannot touch any pixel the frame will draw.
    [[nodiscard]] bool cull_quad(const rhi::Rect& rect);
    void update_push_constants(std::uint32_t state_index);
    // Binds the batch's pipeline when it differs from the one bound (`bound` tracks it across a
    // replay); flush() restores the frame pipeline at the end so later passes see it unchanged.
    void bind_batch_pipeline(const Batch& batch, rhi::Pipeline*& bound);
    // Binds the batch's sampled images: the frame's array on the descriptor-indexing path, one
    // texture on the one-set-per-texture path (RHI 1.7, issue #480 part 2).
    void bind_batch_textures(const Batch& batch);
    // The frame's array slot for `texture`, adding it on first use. Returns false when the frame
    // already holds `texture_array_capacity()` textures, so the caller keeps the one-set path.
    bool frame_texture_slot(rhi::Texture* texture, std::uint32_t& slot);
    void apply_clip_state(const ClipStateTable::Update& update);
    static void resume_pass_callback(void* context);

    // One open push_layer(). An isolated layer owns the pass on its pooled target and remembers the
    // frame target to return to; a flat one only remembers the alpha scale to restore.
    struct LayerFrame {
        bool isolated = false;
        float previous_alpha_scale = 1.0f;
        float opacity = 1.0f;
        const rhi::RenderTarget* parent_target = nullptr;
        rhi::RenderTargetDesc parent_desc;
        rhi::RenderTarget* layer_target = nullptr;
        rhi::RenderTargetDesc layer_desc;
        rhi::Scissor region;
        // The open mask's target, the open mask layer's target, and whether the layer was masked.
        rhi::RenderTarget* mask_target = nullptr;
        rhi::RenderTarget* part_target = nullptr;
        MaskCompositeOp part_op = MaskCompositeOp::Add;
        bool masked = false;
        // The layer's pending `filter` and its scratch targets (see run_layer_filters()).
        LayerFilterChain filter;
        rhi::RenderTarget* scratch = nullptr;
        rhi::RenderTarget* spare = nullptr;
        LayerRetainFrame retain; // retained-layer bookkeeping (layer_retention.hpp)
    };
    // Retained layers (batched2d_layer_retain.cpp): the hit path opens and closes a layer that only
    // composites; a recording layer renders its whole box and copies it into the cache at the pop.
    void push_retained_layer(const LayerRetainPending& pending, float opacity);
    void pop_retained_layer(const LayerFrame& layer);
    [[nodiscard]] rhi::RenderTarget* begin_retain_record(const LayerRetainPending& pending,
                                                         const rhi::Scissor& region);
    void finish_retain_record(const LayerFrame& layer);
    [[nodiscard]] std::uint32_t degradations() const;
    // Runs the layer's pending filter chain once, before the mask or the composite. When it ran,
    // the layer pass is left closed (pass_broken_) on the layer's possibly replaced target.
    void run_layer_filters(LayerFrame& layer);
    // The scissors a layer composites (or is masked) through: the frame clip intersected with the
    // layer's region, and with each damage region when the frame is damage-clipped.
    [[nodiscard]] std::vector<rhi::Scissor> layer_scissors(const LayerFrame& layer) const;
    // Device-space pixel bounds of a paint-space box under the active transform and content scale,
    // clamped to the frame.
    [[nodiscard]] rhi::Scissor device_region(const rhi::Rect& box) const;

    rhi::Device* device_ = nullptr;
    rhi::Pipeline* pipeline_ = nullptr;
    rhi::Pipeline* lcd_pipeline_ = nullptr;
    // Descriptor-indexing pipeline (RHI 1.7, issue #480 part 2); null keeps one set per texture.
    rhi::Pipeline* array_pipeline_ = nullptr;
    // Pipeline the next emit_quad() belongs to (null = frame pipeline); set around LCD quads only.
    rhi::Pipeline* quad_pipeline_ = nullptr;
    rhi::Texture* white_texture_ = nullptr;
    rhi::CommandList* commands_ = nullptr;

    // Isolated-layer seam state: the frame target the pass was opened on (kept so resume_pass() can
    // reopen it).
    const rhi::RenderTarget* frame_target_ = nullptr;
    rhi::RenderTargetDesc frame_desc_;
    std::uint64_t frame_index_ = 0;
    bool pass_broken_ = false;

    GlassCompositor glass_;
    LayerRetainState retain_;
    std::vector<LayerFrame> layers_;
    // Product of the opacities of the open flat layers, folded into every emitted colour.
    float alpha_scale_ = 1.0f;
    std::uint32_t layers_isolated_ = 0;
    std::uint32_t layers_flat_ = 0;
    std::uint32_t layer_masks_applied_ = 0;
    std::uint32_t layer_masks_unsupported_ = 0;
    std::uint32_t layer_filters_applied_ = 0;
    std::uint32_t layer_filters_unsupported_ = 0;
    ClipStateTable clips_;
    // Scaled vertices of the shape being pushed, reused so a content-scaled push allocates nothing
    // once it has grown to the largest polygon.
    std::vector<clip::ClipVertex> shape_scratch_;
    // The per-frame clip-record buffer the fragment shader reads (ADR-068 slice 3). Empty and
    // unused on a device without storage buffers, where the push-constant path stays.
    ClipBuffer clip_buffer_;
    // The frame's gradient records and ramps, uploaded into the same buffer (ADR-076).
    GradientTable gradients_;
    std::uint32_t gradient_quads_ = 0;
    std::uint32_t gradient_fallbacks_ = 0;
    // The pipeline the command list has bound, as far as this renderer knows. Null after anything
    // that binds outside flush() (begin, resume_pass, a glass composite), so the next flush
    // rebinds.
    rhi::Pipeline* bound_pipeline_ = nullptr;
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
    BatchSplitStats split_stats_;

    rhi::Mat3 transform_;
    std::vector<rhi::Mat3> transform_stack_;
    // Presented-pixel -> target-pixel factor for this frame; (1, 1) unless a caller downgraded the
    // render target. See set_content_scale().
    float content_scale_x_ = 1.0f;
    float content_scale_y_ = 1.0f;

    std::vector<Batch> batches_;
    Batch current_batch_;
    rhi::Texture* current_texture_ = nullptr;
    // The frame's texture-array slots (RHI 1.7, issue #480 part 2): every distinct texture the
    // frame draws, in first-use order, and the index each vertex carries. The array is bound ONCE
    // per flush, because a descriptor set is read when the GPU executes the draw, not when it is
    // recorded - rewriting one set per batch would make every draw read the last write. Empty on
    // the one-set-per-texture path, and when the frame uses more textures than the device's array
    // holds.
    std::vector<rhi::Texture*> frame_textures_;

    std::uint32_t draw_calls_ = 0;
    bool in_frame_ = false;
};

} // namespace tessera::rendering::gpu
