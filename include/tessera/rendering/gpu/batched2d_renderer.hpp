// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/gpu/clip.hpp"
#include "tessera/rendering/gpu/vertex2d.hpp"
#include "tessera/rendering/redraw_plan.hpp"
#include "tessera/rendering/rhi/device.hpp"

#include <array>
#include <cstdint>
#include <memory>
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
};

class Batched2DRenderer {
public:
    Batched2DRenderer(rhi::Device& device, rhi::Pipeline& pipeline, rhi::Texture& white_texture,
                      Batched2DConfig config = {});
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
    [[nodiscard]] std::uint32_t clip_depth() const {
        return static_cast<std::uint32_t>(clip_stack_.size());
    }

    void set_transform(const rhi::Mat3& transform);
    void push_transform(const rhi::Mat3& transform);
    void pop_transform();

    void push_rect(const rhi::Rect& rect, rhi::Color color);
    void push_rounded_rect(const rhi::Rect& rect, rhi::Color color, float radius);
    void push_texture(const rhi::Rect& rect, rhi::Texture& texture, const rhi::Rect& uv,
                      rhi::Color color);

    void flush();

    [[nodiscard]] std::uint32_t draw_calls() const {
        return draw_calls_;
    }
    [[nodiscard]] std::uint32_t vertex_count() const {
        return emitted_vertices_;
    }
    [[nodiscard]] std::uint32_t pending_batches() const {
        return static_cast<std::uint32_t>(batches_.size() +
                                          (current_batch_.vertex_count > 0 ? 1 : 0));
    }

private:
    // Folded clip uniforms for one batch. `count` clips are live in `clips`; state 0 is always the
    // empty clip set, so a batch only splits when the uniform set (or texture/scissor) changes.
    struct ClipState {
        std::uint32_t count = 0;
        std::array<clip::RoundedRect, clip::kMaxShaderClips> clips{};
    };

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
    void select_next_slot();
    void ensure_buffer_space(std::size_t vertices);
    [[nodiscard]] rhi::Buffer& current_buffer();
    void flush_batch();
    void emit_quad(const rhi::Rect& rect, const rhi::Rect& uv, rhi::Color color, float radius,
                   float mode, rhi::Texture* texture);
    void update_push_constants(std::uint32_t state_index);
    void rebuild_clip_state();
    void apply_clip_state(std::uint32_t state_index, const rhi::Scissor& scissor);
    [[nodiscard]] std::uint32_t intern_clip_state(const ClipState& state);
    static bool same_clip_state(const ClipState& a, const ClipState& b);

    rhi::Device* device_ = nullptr;
    rhi::Pipeline* pipeline_ = nullptr;
    rhi::Texture* white_texture_ = nullptr;
    rhi::CommandList* commands_ = nullptr;

    Batched2DConfig config_;
    rhi::Extent2D extent_;
    rhi::Scissor base_scissor_;
    rhi::Scissor scissor_;
    std::vector<rhi::Scissor> scissor_stack_;
    std::vector<rhi::Scissor> damage_regions_;

    // Rounded clips: the caller-visible stack, its folded shader state, and the interned states of
    // the current frame. Clip states are interned so a batch only stores an index.
    std::vector<clip::RoundedRect> clip_stack_;
    std::vector<bool> clip_folded_;
    std::vector<rhi::Scissor> clip_scissor_stack_;
    std::vector<ClipState> clip_states_;
    std::uint32_t current_clip_state_ = 0;

    rhi::Mat3 transform_;
    std::vector<rhi::Mat3> transform_stack_;
    // Presented-pixel -> target-pixel factor for this frame; (1, 1) unless a caller downgraded the
    // render target. See set_content_scale().
    float content_scale_x_ = 1.0f;
    float content_scale_y_ = 1.0f;

    // Vertex memory for one in-flight frame. A frame normally uses a single buffer; when it runs
    // out of room it continues in a larger one, because draws already recorded this frame still
    // reference the earlier buffers. Extra buffers are dropped when the slot is reused.
    struct FrameSlot {
        std::vector<std::unique_ptr<rhi::Buffer>> buffers;
        std::uint32_t capacity = 0; // vertices in buffers.back()
    };
    std::vector<FrameSlot> slots_;
    std::uint32_t slot_index_ = 0;
    std::uint32_t buffer_used_ = 0; // vertices already written to the current buffer this frame
    std::vector<Vertex2D> vertices_;
    std::vector<Batch> batches_;
    Batch current_batch_;
    rhi::Texture* current_texture_ = nullptr;

    std::uint32_t draw_calls_ = 0;
    std::uint32_t emitted_vertices_ = 0;
    bool in_frame_ = false;
};

} // namespace tessera::rendering::gpu
